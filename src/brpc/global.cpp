// Copyright (c) 2014 baidu-rpc authors.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: Ge,Jun (gejun@baidu.com)

#include <openssl/ssl.h>
#include <gflags/gflags.h>
#include <fcntl.h>                               // O_RDONLY
#include <signal.h>

// Naming services
#ifdef BAIDU_INTERNAL
#include "brpc/policy/baidu_naming_service.h"
#endif
#include "brpc/policy/file_naming_service.h"
#include "brpc/policy/list_naming_service.h"
#include "brpc/policy/domain_naming_service.h"
#include "brpc/policy/remote_file_naming_service.h"

// Load Balancers
#include "brpc/policy/round_robin_load_balancer.h"
#include "brpc/policy/randomized_load_balancer.h"
#include "brpc/policy/locality_aware_load_balancer.h"
#include "brpc/policy/consistent_hashing_load_balancer.h"
#include "brpc/policy/hasher.h"
#include "brpc/policy/dynpart_load_balancer.h"

// Compress handlers
#include "brpc/compress.h"
#include "brpc/policy/gzip_compress.h"
#include "brpc/policy/snappy_compress.h"

// Protocols
#include "brpc/protocol.h"
#include "brpc/policy/baidu_rpc_protocol.h"
#include "brpc/policy/http_rpc_protocol.h"
#include "brpc/policy/hulu_pbrpc_protocol.h"
#include "brpc/policy/nova_pbrpc_protocol.h"
#include "brpc/policy/public_pbrpc_protocol.h"
#include "brpc/policy/ubrpc2pb_protocol.h"
#include "brpc/policy/sofa_pbrpc_protocol.h"
#include "brpc/policy/memcache_binary_protocol.h"
#include "brpc/policy/streaming_rpc_protocol.h"
#include "brpc/policy/mongo_protocol.h"
#include "brpc/policy/redis_protocol.h"
#include "brpc/policy/nshead_mcpack_protocol.h"
#include "brpc/policy/rtmp_protocol.h"
#include "brpc/policy/esp_protocol.h"

#include "brpc/input_messenger.h"     // get_or_new_client_side_messenger
#include "brpc/socket_map.h"          // SocketMapList
#include "brpc/server.h"
#include "brpc/trackme.h"             // TrackMe
#include <malloc.h>                        // malloc_trim
#include "brpc/details/usercode_backup_pool.h"
#include "base/fd_guard.h"
#include "base/files/file_watcher.h"

namespace brpc {

DECLARE_bool(usercode_in_pthread);

namespace policy {
// Defined in http_rpc_protocol.cpp
void InitCommonStrings();
}

using namespace policy;

const char* const DUMMY_SERVER_PORT_FILE = "dummy_server.port";

struct GlobalExtensions {
    GlobalExtensions()
        : ch_mh_lb(MurmurHash32)
        , ch_md5_lb(MD5Hash32){}
#ifdef BAIDU_INTERNAL
    BaiduNamingService bns;
#endif
    FileNamingService fns;
    ListNamingService lns;
    DomainNamingService dns;
    RemoteFileNamingService rfns;

    RoundRobinLoadBalancer rr_lb;
    RandomizedLoadBalancer randomized_lb;
    LocalityAwareLoadBalancer la_lb;
    ConsistentHashingLoadBalancer ch_mh_lb;
    ConsistentHashingLoadBalancer ch_md5_lb;
    DynPartLoadBalancer dynpart_lb;
};

static pthread_once_t register_extensions_once = PTHREAD_ONCE_INIT;
static GlobalExtensions* g_ext = NULL;

static long ReadPortOfDummyServer(const char* filename) {
    base::fd_guard fd(open(filename, O_RDONLY));
    if (fd < 0) {
        LOG(ERROR) << "Fail to open `" << DUMMY_SERVER_PORT_FILE << "'";
        return -1;
    }
    char port_str[32];
    const ssize_t nr = read(fd, port_str, sizeof(port_str));
    if (nr <= 0) {
        LOG(ERROR) << "Fail to read `" << DUMMY_SERVER_PORT_FILE << "': "
                   << (nr == 0 ? "nothing to read" : berror());
        return -1;
    }
    port_str[sizeof(port_str)-1] = '\0';
    const char* p = port_str;
    for (; isspace(*p); ++p) {}
    char* endptr = NULL;
    const long port = strtol(p, &endptr, 10);
    for (; isspace(*endptr); ++endptr) {}
    if (*endptr != '\0') {
        LOG(ERROR) << "Invalid port=`" << port_str << "'";
        return -1;
    }
    return port;
}

// Expose counters of base::IOBuf
static int64_t GetIOBufBlockCount(void*) {
    return base::IOBuf::block_count();
}
static int64_t GetIOBufBlockCountHitTLSThreshold(void*) {
    return base::IOBuf::block_count_hit_tls_threshold();
}
static int64_t GetIOBufNewBigViewCount(void*) {
    return base::IOBuf::new_bigview_count();
}
static int64_t GetIOBufBlockMemory(void*) {
    return base::IOBuf::block_memory();
}

// Defined in server.cpp
extern base::static_atomic<int> g_running_server_count;
static int GetRunningServerCount(void*) {
    return g_running_server_count.load(base::memory_order_relaxed);
}
    
// Update global stuff periodically.
static void* GlobalUpdate(void*) {
    // Expose variables.
    bvar::PassiveStatus<int64_t> var_iobuf_block_count(
        "iobuf_block_count", GetIOBufBlockCount, NULL);
    bvar::PassiveStatus<int64_t> var_iobuf_block_count_hit_tls_threshold(
        "iobuf_block_count_hit_tls_threshold",
        GetIOBufBlockCountHitTLSThreshold, NULL);
    bvar::PassiveStatus<int64_t> var_iobuf_new_bigview_count(
        GetIOBufNewBigViewCount, NULL);
    bvar::PerSecond<bvar::PassiveStatus<int64_t> > var_iobuf_new_bigview_second(
        "iobuf_newbigview_second", &var_iobuf_new_bigview_count);
    bvar::PassiveStatus<int64_t> var_iobuf_block_memory(
        "iobuf_block_memory", GetIOBufBlockMemory, NULL);
    bvar::PassiveStatus<int> var_running_server_count(
        "rpc_server_count", GetRunningServerCount, NULL);
    
    base::FileWatcher fw;
    if (fw.init_from_not_exist(DUMMY_SERVER_PORT_FILE) < 0) {
        LOG(FATAL) << "Fail to init FileWatcher on `" << DUMMY_SERVER_PORT_FILE << "'";
        return NULL;
    }

    std::vector<SocketId> conns;
    const int64_t start_time_us = base::gettimeofday_us();
    const int WARN_NOSLEEP_THRESHOLD = 2;
    int64_t last_time_us = start_time_us;
    int consecutive_nosleep = 0;
    //int64_t last_malloc_trim_time = start_time_us;
    while (1) {
        const int64_t sleep_us = 1000000L + last_time_us - base::gettimeofday_us();
        if (sleep_us > 0) {
            if (bthread_usleep(sleep_us) < 0) {
                PLOG_IF(FATAL, errno != ESTOP) << "Fail to sleep";
                break;
            }
            consecutive_nosleep = 0;
        } else {            
            if (++consecutive_nosleep >= WARN_NOSLEEP_THRESHOLD) {
                consecutive_nosleep = 0;
                LOG(WARNING) << __FUNCTION__ << " is too busy!";
            }
        }
        last_time_us = base::gettimeofday_us();
        
        TrackMe();
        
        if (!IsDummyServerRunning()
            && g_running_server_count.load(base::memory_order_relaxed) == 0
            && fw.check_and_consume() > 0) {
            long port = ReadPortOfDummyServer(DUMMY_SERVER_PORT_FILE);
            if (port >= 0) {
                StartDummyServerAt(port);
            }
        }

        SocketMapList(&conns);
        const int64_t now_ms = base::cpuwide_time_ms();
        for (size_t i = 0; i < conns.size(); ++i) {
            SocketUniquePtr ptr;
            if (Socket::Address(conns[i], &ptr) == 0) {
                ptr->UpdateStatsEverySecond(now_ms);
            }
        }

        // TODO: Add branch for tcmalloc.
        // if (last_time_us > last_malloc_trim_time + 10*1000000L) {
        //     last_malloc_trim_time = last_time_us;
        //     malloc_trim(10*1024*1024/*leave 10M pad*/);
        // }
    }
    return NULL;
}

static void BaiduStreamingLogHandler(google::protobuf::LogLevel level,
                                     const char* filename, int line,
                                     const std::string& message) {
    switch (level) {
    case google::protobuf::LOGLEVEL_INFO:
        LOG_AT(INFO, filename, line) << message;
        return;
    case google::protobuf::LOGLEVEL_WARNING:
        LOG_AT(WARNING, filename, line) << message;
        return;
    case google::protobuf::LOGLEVEL_ERROR:
        LOG_AT(ERROR, filename, line) << message;
        return;
    case google::protobuf::LOGLEVEL_FATAL:
        LOG_AT(FATAL, filename, line) << message;
        return;
    }
    LOG_AT(FATAL, filename, line) << message;
}

static void GlobalInitializeOrDieImpl() {
    //////////////////////////////////////////////////////////////////
    // Be careful about usages of gflags inside this function which //
    // may be called before main() only seeing gflags with default  //
    // values even if the gflags will be set after main().          //
    //////////////////////////////////////////////////////////////////
    
    // Ignore SIGPIPE. 
    struct sigaction oldact;
    if (sigaction(SIGPIPE, NULL, &oldact) != 0 || 
            (oldact.sa_handler == NULL && oldact.sa_sigaction == NULL)) {
        CHECK(NULL == signal(SIGPIPE, SIG_IGN));
    }
    
    // Make GOOGLE_LOG print to comlog device
    SetLogHandler(&BaiduStreamingLogHandler);

    // Setting the variable here does not work, the profiler probably check
    // the variable before main() for only once.
    // setenv("TCMALLOC_SAMPLE_PARAMETER", "524288", 0);

    // Initialize openssl library
    SSL_library_init();
    SSL_load_error_strings();
    if (SSLThreadInit() != 0 || SSLDHInit() != 0) {
        exit(1);
    }

    // Defined in http_rpc_protocol.cpp
    InitCommonStrings();
    
    // Leave memory of these extensions to process's clean up.
    g_ext = new(std::nothrow) GlobalExtensions();
    if (NULL == g_ext) {
        exit(1);
    }
    // Naming Services
#ifdef BAIDU_INTERNAL
    NamingServiceExtension()->RegisterOrDie("bns", &g_ext->bns);
#endif
    NamingServiceExtension()->RegisterOrDie("file", &g_ext->fns);
    NamingServiceExtension()->RegisterOrDie("list", &g_ext->lns);
    NamingServiceExtension()->RegisterOrDie("http", &g_ext->dns);
    NamingServiceExtension()->RegisterOrDie("remotefile", &g_ext->rfns);

    // Load Balancers
    LoadBalancerExtension()->RegisterOrDie("rr", &g_ext->rr_lb);
    LoadBalancerExtension()->RegisterOrDie("random", &g_ext->randomized_lb);
    LoadBalancerExtension()->RegisterOrDie("la", &g_ext->la_lb);
    LoadBalancerExtension()->RegisterOrDie("c_murmurhash", &g_ext->ch_mh_lb);
    LoadBalancerExtension()->RegisterOrDie("c_md5", &g_ext->ch_md5_lb);
    LoadBalancerExtension()->RegisterOrDie("_dynpart", &g_ext->dynpart_lb);

    // Compress Handlers
    const CompressHandler gzip_compress =
        { GzipCompress, GzipDecompress, "gzip" };
    if (RegisterCompressHandler(COMPRESS_TYPE_GZIP, gzip_compress) != 0) {
        exit(1);
    }
    const CompressHandler zlib_compress =
        { ZlibCompress, ZlibDecompress, "zlib" };
    if (RegisterCompressHandler(COMPRESS_TYPE_ZLIB, zlib_compress) != 0) {
        exit(1);
    }
    const CompressHandler snappy_compress =
        { SnappyCompress, SnappyDecompress, "snappy" };
    if (RegisterCompressHandler(COMPRESS_TYPE_SNAPPY, snappy_compress) != 0) {
        exit(1);
    }

    // Protocols
    Protocol baidu_protocol = { ParseRpcMessage,
                                SerializeRequestDefault, PackRpcRequest,
                                ProcessRpcRequest, ProcessRpcResponse,
                                VerifyRpcRequest, NULL, NULL,
                                CONNECTION_TYPE_ALL, "baidu_std" };
    if (RegisterProtocol(PROTOCOL_BAIDU_STD, baidu_protocol) != 0) {
        exit(1);
    }

    Protocol streaming_protocol = { ParseStreamingMessage,
                                    NULL, NULL, ProcessStreamingMessage, 
                                    ProcessStreamingMessage,
                                    NULL, NULL, NULL,
                                    CONNECTION_TYPE_SINGLE, "streaming_rpc" };

    if (RegisterProtocol(PROTOCOL_STREAMING_RPC, streaming_protocol) != 0) {
        exit(1);
    }
    
    Protocol http_protocol = { ParseHttpMessage,
                               SerializeHttpRequest, PackHttpRequest,
                               ProcessHttpRequest, ProcessHttpResponse,
                               VerifyHttpRequest, ParseHttpServerAddress,
                               GetHttpMethodName,
                               CONNECTION_TYPE_POOLED_AND_SHORT,
                               "http" };
    if (RegisterProtocol(PROTOCOL_HTTP, http_protocol) != 0) {
        exit(1);
    }
    
    Protocol hulu_protocol = { ParseHuluMessage,
                               SerializeRequestDefault, PackHuluRequest,
                               ProcessHuluRequest, ProcessHuluResponse,
                               VerifyHuluRequest, NULL, NULL,
                               CONNECTION_TYPE_ALL, "hulu_pbrpc" };
    if (RegisterProtocol(PROTOCOL_HULU_PBRPC, hulu_protocol) != 0) {
        exit(1);
    }

    // Only valid at client side
    Protocol nova_protocol = { ParseNsheadMessage,
                               SerializeNovaRequest, PackNovaRequest,
                               NULL, ProcessNovaResponse,
                               NULL, NULL, NULL,
                               CONNECTION_TYPE_POOLED_AND_SHORT,  "nova_pbrpc" };
    if (RegisterProtocol(PROTOCOL_NOVA_PBRPC, nova_protocol) != 0) {
        exit(1);
    }

    // Only valid at client side
    Protocol public_pbrpc_protocol = { ParseNsheadMessage,
                                       SerializePublicPbrpcRequest,
                                       PackPublicPbrpcRequest,
                                       NULL, ProcessPublicPbrpcResponse,
                                       NULL, NULL, NULL,
                                       // public/pbrpc server implementation
                                       // doesn't support full duplex
                                       CONNECTION_TYPE_POOLED_AND_SHORT,
                                       "public_pbrpc" };
    if (RegisterProtocol(PROTOCOL_PUBLIC_PBRPC, public_pbrpc_protocol) != 0) {
        exit(1);
    }

    Protocol sofa_protocol = { ParseSofaMessage,
                               SerializeRequestDefault, PackSofaRequest,
                               ProcessSofaRequest, ProcessSofaResponse,
                               VerifySofaRequest, NULL, NULL,
                               CONNECTION_TYPE_ALL, "sofa_pbrpc" };
    if (RegisterProtocol(PROTOCOL_SOFA_PBRPC, sofa_protocol) != 0) {
        exit(1);
    }

    // Only valid at server side. We generalize all the protocols that 
    // prefixes with nshead as `nshead_protocol' and specify the content
    // parsing after nshead by ServerOptions.nshead_service.
    Protocol nshead_protocol = { ParseNsheadMessage,
                                 SerializeNsheadRequest, PackNsheadRequest,
                                 ProcessNsheadRequest, ProcessNsheadResponse,
                                 VerifyNsheadRequest, NULL, NULL,
                                 CONNECTION_TYPE_POOLED_AND_SHORT, "nshead" };
    if (RegisterProtocol(PROTOCOL_NSHEAD, nshead_protocol) != 0) {
        exit(1);
    }

    Protocol mc_binary_protocol = { ParseMemcacheMessage,
                                    SerializeMemcacheRequest,
                                    PackMemcacheRequest,
                                    NULL, ProcessMemcacheResponse,
                                    NULL, NULL, GetMemcacheMethodName,
                                    CONNECTION_TYPE_ALL, "memcache" };
    if (RegisterProtocol(PROTOCOL_MEMCACHE, mc_binary_protocol) != 0) {
        exit(1);
    }

    Protocol redis_protocol = { ParseRedisMessage,
                                SerializeRedisRequest,
                                PackRedisRequest,
                                NULL, ProcessRedisResponse,
                                NULL, NULL, GetRedisMethodName,
                                CONNECTION_TYPE_ALL, "redis" };
    if (RegisterProtocol(PROTOCOL_REDIS, redis_protocol) != 0) {
        exit(1);
    }

    Protocol mongo_protocol = { ParseMongoMessage,
                                NULL, NULL,
                                ProcessMongoRequest, NULL,
                                NULL, NULL, NULL,
                                CONNECTION_TYPE_POOLED, "mongo" };
    if (RegisterProtocol(PROTOCOL_MONGO, mongo_protocol) != 0) {
        exit(1);
    }

    // Only valid at client side
    Protocol ubrpc_compack_protocol = {
        ParseNsheadMessage,
        SerializeUbrpcCompackRequest, PackUbrpcRequest,
        NULL, ProcessUbrpcResponse,
        NULL, NULL, NULL,
        CONNECTION_TYPE_POOLED_AND_SHORT,  "ubrpc_compack" };
    if (RegisterProtocol(PROTOCOL_UBRPC_COMPACK, ubrpc_compack_protocol) != 0) {
        exit(1);
    }
    Protocol ubrpc_mcpack2_protocol = {
        ParseNsheadMessage,
        SerializeUbrpcMcpack2Request, PackUbrpcRequest,
        NULL, ProcessUbrpcResponse,
        NULL, NULL, NULL,
        CONNECTION_TYPE_POOLED_AND_SHORT,  "ubrpc_mcpack2" };
    if (RegisterProtocol(PROTOCOL_UBRPC_MCPACK2, ubrpc_mcpack2_protocol) != 0) {
        exit(1);
    }

    // Only valid at client side
    Protocol nshead_mcpack_protocol = {
        ParseNsheadMessage,
        SerializeNsheadMcpackRequest, PackNsheadMcpackRequest,
        NULL, ProcessNsheadMcpackResponse,
        NULL, NULL, NULL,
        CONNECTION_TYPE_POOLED_AND_SHORT,  "nshead_mcpack" };
    if (RegisterProtocol(PROTOCOL_NSHEAD_MCPACK, nshead_mcpack_protocol) != 0) {
        exit(1);
    }
    
    Protocol rtmp_protocol = {
        ParseRtmpMessage,
        SerializeRtmpRequest, PackRtmpRequest,
        ProcessRtmpMessage, ProcessRtmpMessage,
        NULL, NULL, NULL,
        (ConnectionType)(CONNECTION_TYPE_SINGLE|CONNECTION_TYPE_SHORT),
        "rtmp" };
    if (RegisterProtocol(PROTOCOL_RTMP, rtmp_protocol) != 0) {
        exit(1);
    }

    Protocol esp_protocol = {
        ParseEspMessage,
        SerializeEspRequest, PackEspRequest,
        NULL, ProcessEspResponse,
        NULL, NULL, NULL,
        CONNECTION_TYPE_POOLED_AND_SHORT, "esp"};
    if (RegisterProtocol(PROTOCOL_ESP, esp_protocol) != 0) {
        exit(1);
    }

    std::vector<Protocol> protocols;
    ListProtocols(&protocols);
    for (size_t i = 0; i < protocols.size(); ++i) {
        if (protocols[i].process_response) {
            InputMessageHandler handler;
            // `process_response' is required at client side
            handler.parse = protocols[i].parse;
            handler.process = protocols[i].process_response;
            // No need to verify at client side
            handler.verify = NULL;
            handler.arg = NULL;
            handler.name = protocols[i].name;
            if (get_or_new_client_side_messenger()->AddHandler(handler) != 0) {
                exit(1);
            }
        }
    }

    if (FLAGS_usercode_in_pthread) {
        // Optional. If channel/server are initialized before main(), this
        // flag may be false at here even if it will be set to true after
        // main(). In which case, the usercode pool will not be initialized
        // until the pool is used.
        InitUserCodeBackupPoolOnceOrDie();
    }

    // We never join GlobalUpdate, let it quit with the process. 
    bthread_t th;
    CHECK(bthread_start_background(&th, NULL, GlobalUpdate, NULL) == 0)
        << "Fail to start GlobalUpdate";
}

void GlobalInitializeOrDie() {
    if (pthread_once(&register_extensions_once,
                     GlobalInitializeOrDieImpl) != 0) {
        LOG(FATAL) << "Fail to pthread_once";
        exit(1);
    }
}

} // namespace brpc
