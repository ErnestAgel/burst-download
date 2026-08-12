/**
 * @file test_curlmulti.cpp
 * @brief Tests for the shared curl_multi engine (P8-4).
 *
 * The engine is verified against a local HTTP server on 127.0.0.1, so the
 * tests are fully offline.  The server serves a deterministic byte pattern
 * with Range support and paces its writes so several transfers overlap,
 * which lets the tests assert true connection-level parallelism.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "curlmulti.h"
#include "test_framework.h"

#ifdef _WIN32
typedef SOCKET TSocket;
static const TSocket kInvalidSocket = INVALID_SOCKET;
static void CloseSocket(TSocket s) { closesocket(s); }
#else
typedef int TSocket;
static const TSocket kInvalidSocket = -1;
static void CloseSocket(TSocket s) { close(s); }
#endif

namespace
{

#ifdef _WIN32
/** @brief One-time Winsock initialization for the test process. */
struct TWinSockInit
{
    TWinSockInit()
    {
        WSADATA tData;
        WSAStartup(MAKEWORD(2, 2), &tData);
    }
    ~TWinSockInit() { WSACleanup(); }
};
TWinSockInit g_tWinSockInit;
#endif

/** @brief Byte at a file position (server and client must agree). */
static u8 PatternByte(u64 dwPos)
{
    return static_cast<u8>((dwPos * 131u + 17u) & 0xFFu);
}

/** @brief Case-insensitive substring search. */
static size_t FindCaseInsensitive(const std::string& strHay,
                                  const std::string& strNeedle)
{
    if (strNeedle.empty())
    {
        return std::string::npos;
    }
    if (strHay.size() < strNeedle.size())
    {
        return std::string::npos;
    }
    const size_t nLimit = strHay.size() - strNeedle.size() + 1u;
    for (size_t nIndex = 0; nIndex < nLimit; ++nIndex)
    {
        BOOL32 bMatch = TRUE;
        for (size_t n = 0; n < strNeedle.size(); ++n)
        {
            char a = strHay[nIndex + n];
            char b = strNeedle[n];
            if ((a >= 'A') && (a <= 'Z'))
            {
                a = static_cast<char>(a + 32);
            }
            if ((b >= 'A') && (b <= 'Z'))
            {
                b = static_cast<char>(b + 32);
            }
            if (a != b)
            {
                bMatch = FALSE;
                break;
            }
        }
        if (bMatch != FALSE)
        {
            return nIndex;
        }
    }
    return std::string::npos;
}

/**
 * @brief Minimal local HTTP server (thread per connection) with Range
 *        support, a /fail endpoint and a /slow endpoint.
 */
class CLocalHttpServer
{
public:
    CLocalHttpServer() = default;
    ~CLocalHttpServer() { Stop(); }

    CLocalHttpServer(const CLocalHttpServer&) = delete;
    CLocalHttpServer& operator=(const CLocalHttpServer&) = delete;

    /**
     * @brief Bind 127.0.0.1 on an ephemeral port and start accepting.
     * @param dwFileSize File size served for /file.
     * @param dwPaceUs Sleep between 4 KiB body writes (overlap control).
     */
    BOOL32 Start(u32 dwFileSize, u32 dwPaceUs);

    /** @brief URL of the regular /file endpoint. */
    std::string Url() const { return m_strUrl; }

    /** @brief URL of the /fail endpoint (always HTTP 500). */
    std::string FailUrl() const { return m_strFailUrl; }

    /** @brief Peak number of simultaneously connected clients. */
    u32 MaxConcurrent() const { return m_dwMaxConcurrent.load(); }

    /** @brief Stop accepting, close the listener and join the acceptor. */
    void Stop();

private:
    /** @brief Accept loop: one detached thread per connection. */
    void AcceptLoop();

    /** @brief Serve one connection (blocking; checks the stop flag). */
    void HandleClient(TSocket sock);

    /** @brief Send a buffer fully; FALSE on a socket error. */
    BOOL32 SendAll(TSocket sock, const char* pszData, size_t nLen) const;

    TSocket m_sock = kInvalidSocket;
    u32 m_dwFileSize = 0u;
    u32 m_dwPaceUs = 0u;
    std::string m_strUrl;
    std::string m_strFailUrl;
    std::atomic<BOOL32> m_bStop{FALSE};
    std::atomic<u32> m_dwActiveClients{0u};
    std::atomic<u32> m_dwMaxConcurrent{0u};
    std::thread m_thAccept;
};

BOOL32 CLocalHttpServer::Start(u32 dwFileSize, u32 dwPaceUs)
{
    m_dwFileSize = dwFileSize;
    m_dwPaceUs = dwPaceUs;
    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock == kInvalidSocket)
    {
        return FALSE;
    }

    int nReuse = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&nReuse), sizeof(nReuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  /* ephemeral */
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        CloseSocket(m_sock);
        m_sock = kInvalidSocket;
        return FALSE;
    }

    sockaddr_in addrBound;
    std::memset(&addrBound, 0, sizeof(addrBound));
#ifdef _WIN32
    int nLen = sizeof(addrBound);
#else
    socklen_t nLen = sizeof(addrBound);
#endif
    if (getsockname(m_sock, reinterpret_cast<sockaddr*>(&addrBound),
                    &nLen) != 0)
    {
        CloseSocket(m_sock);
        m_sock = kInvalidSocket;
        return FALSE;
    }

    if (listen(m_sock, 16) != 0)
    {
        CloseSocket(m_sock);
        m_sock = kInvalidSocket;
        return FALSE;
    }

    char szPort[16];
    snprintf(szPort, sizeof(szPort), "%u",
             static_cast<unsigned>(ntohs(addrBound.sin_port)));
    const std::string strBase = "http://127.0.0.1:" + std::string(szPort);
    m_strUrl = strBase + "/file";
    m_strFailUrl = strBase + "/fail";

    m_bStop.store(FALSE);
    m_thAccept = std::thread(&CLocalHttpServer::AcceptLoop, this);
    return TRUE;
}

void CLocalHttpServer::Stop()
{
    if (m_bStop.exchange(TRUE) != FALSE)
    {
        return;
    }
    if (m_sock != kInvalidSocket)
    {
        CloseSocket(m_sock);
        m_sock = kInvalidSocket;
    }
    if (m_thAccept.joinable())
    {
        m_thAccept.join();
    }
    /* Give detached client threads a bounded window to drain. */
    for (int nWait = 0; nWait < 200; ++nWait)
    {
        if (m_dwActiveClients.load() == 0u)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void CLocalHttpServer::AcceptLoop()
{
    for (;;)
    {
        /* Poll the listener with a bounded timeout so Stop() (which closes
         * the socket) is observed promptly: on Linux, closing a listening
         * socket does not wake a blocking accept(), while Windows'
         * closesocket() does.  select() with a short timeout is portable. */
        if (m_bStop.load() != FALSE)
        {
            break;
        }
        fd_set setRead;
        FD_ZERO(&setRead);
        FD_SET(m_sock, &setRead);
        timeval tTimeout;
        tTimeout.tv_sec = 0;
        tTimeout.tv_usec = 200000;  /* 200 ms */
        const int nReady = select(m_sock + 1, &setRead, nullptr, nullptr,
                                  &tTimeout);
        if (nReady <= 0)
        {
            continue;  /* timeout or listener closed: re-check the flag */
        }

        sockaddr_in addrClient;
        std::memset(&addrClient, 0, sizeof(addrClient));
#ifdef _WIN32
        int nLen = sizeof(addrClient);
#else
        socklen_t nLen = sizeof(addrClient);
#endif
        const TSocket s =
            accept(m_sock, reinterpret_cast<sockaddr*>(&addrClient), &nLen);
        if (s == kInvalidSocket)
        {
            break;  /* listener closed by Stop() */
        }
        if (m_bStop.load() != FALSE)
        {
            CloseSocket(s);
            break;
        }
        const u32 dwNow = m_dwActiveClients.fetch_add(1u) + 1u;
        u32 dwMax = m_dwMaxConcurrent.load(std::memory_order_relaxed);
        while ((dwNow > dwMax) &&
               !m_dwMaxConcurrent.compare_exchange_weak(
                   dwMax, dwNow, std::memory_order_relaxed))
        {
        }
        std::thread thClient(&CLocalHttpServer::HandleClient, this, s);
        thClient.detach();
    }
}

BOOL32 CLocalHttpServer::SendAll(TSocket sock, const char* pszData,
                                 size_t nLen) const
{
    size_t nSent = 0u;
    while (nSent < nLen)
    {
        const int n = send(sock, pszData + nSent,
                           static_cast<int>(nLen - nSent), 0);
        if (n <= 0)
        {
            return FALSE;
        }
        nSent += static_cast<size_t>(n);
    }
    return TRUE;
}

void CLocalHttpServer::HandleClient(TSocket sock)
{
    std::string strReq;
    char szBuf[4096];
    for (;;)
    {
        const int n = recv(sock, szBuf, sizeof(szBuf), 0);
        if (n <= 0)
        {
            break;
        }
        strReq.append(szBuf, static_cast<size_t>(n));
        if (strReq.find("\r\n\r\n") != std::string::npos)
        {
            break;
        }
        if (strReq.size() > 65536u)
        {
            break;
        }
    }

    if (strReq.find("/slow") != std::string::npos)
    {
        /* Stay open until the test tears the transfer down. */
        while (m_bStop.load() == FALSE)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    else if (strReq.find("/fail") != std::string::npos)
    {
        const std::string strResp =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        SendAll(sock, strResp.data(), strResp.size());
    }
    else
    {
        u64 dwStart = 0u;
        u64 dwEnd = static_cast<u64>(m_dwFileSize) - 1u;
        BOOL32 bRange = FALSE;
        const size_t nPos = FindCaseInsensitive(strReq, "range:");
        if (nPos != std::string::npos)
        {
            const std::string strLine = strReq.substr(nPos);
            const size_t nBytes = strLine.find("bytes=");
            if (nBytes != std::string::npos)
            {
                unsigned long long ullFirst = 0ull;
                unsigned long long ullLast = 0ull;
                if (sscanf(strLine.c_str() + nBytes + 6u, "%llu-%llu",
                           &ullFirst, &ullLast) == 2)
                {
                    bRange = TRUE;
                    dwStart = static_cast<u64>(ullFirst);
                    if (static_cast<u64>(ullLast) < dwEnd)
                    {
                        dwEnd = static_cast<u64>(ullLast);
                    }
                }
            }
        }
        if (dwEnd >= static_cast<u64>(m_dwFileSize))
        {
            dwEnd = static_cast<u64>(m_dwFileSize) - 1u;
        }
        const u64 dwLen = (dwStart <= dwEnd) ? (dwEnd - dwStart + 1u) : 0u;

        char szHead[256];
        if ((bRange != FALSE) && (dwLen > 0u))
        {
            snprintf(szHead, sizeof(szHead),
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Range: bytes %llu-%llu/%u\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     static_cast<unsigned long long>(dwStart),
                     static_cast<unsigned long long>(dwEnd), m_dwFileSize,
                     static_cast<unsigned long long>(dwLen));
        }
        else
        {
            snprintf(szHead, sizeof(szHead),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     m_dwFileSize);
        }
        SendAll(sock, szHead, std::strlen(szHead));

        u64 dwSent = 0u;
        std::vector<u8> vecBody(4096u);
        while ((dwSent < dwLen) && (m_bStop.load() == FALSE))
        {
            size_t m = 4096u;
            if (static_cast<u64>(m) > dwLen - dwSent)
            {
                m = static_cast<size_t>(dwLen - dwSent);
            }
            for (size_t n = 0; n < m; ++n)
            {
                vecBody[n] = PatternByte(dwStart + dwSent + n);
            }
            if (SendAll(sock, reinterpret_cast<const char*>(vecBody.data()),
                        m) == FALSE)
            {
                break;
            }
            dwSent += m;
            if (m_dwPaceUs > 0u)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(m_dwPaceUs));
            }
        }
    }

    CloseSocket(sock);
    m_dwActiveClients.fetch_sub(1u);
}

/** @brief Per-chunk state for the engine tests. */
typedef struct tagTestChunk
{
    std::vector<u8>* pBuffer;  /**< Shared file buffer */
    u64 dwStart;               /**< Chunk start offset */
    u64 dwEnd;                 /**< Chunk end offset */
    std::atomic<u64> dwOffset{0u};      /**< Bytes written so far */
    std::atomic<CURLcode> code{};
    std::atomic<long> lHttp{0L};
    std::atomic<BOOL32> bDone{FALSE};
} TTestChunk;

/** @brief libcurl write callback for the tests. */
static size_t TestWriteCb(char* ptr, size_t size, size_t nmemb,
                          void* userdata)
{
    TTestChunk* pChunk = static_cast<TTestChunk*>(userdata);
    const size_t n = size * nmemb;
    const u64 dwRangeLen = pChunk->dwEnd - pChunk->dwStart + 1u;
    const u64 dwOffset = pChunk->dwOffset.load();
    if (dwOffset < dwRangeLen)
    {
        size_t m = n;
        if (dwOffset + static_cast<u64>(n) > dwRangeLen)
        {
            m = static_cast<size_t>(dwRangeLen - dwOffset);
        }
        std::memcpy(pChunk->pBuffer->data() + pChunk->dwStart + dwOffset,
                    ptr, m);
        pChunk->dwOffset.fetch_add(m);
    }
    return n;
}

/** @brief Build a configured easy handle for one test chunk. */
static CURL* CreateTestEasy(const std::string& strUrl, TTestChunk* pChunk,
                            const std::string& strRange)
{
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        return nullptr;
    }
    curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, TestWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, pChunk);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (strRange.empty() == false)
    {
        curl_easy_setopt(curl, CURLOPT_RANGE, strRange.c_str());
    }
    return curl;
}

/** @brief Wait (bounded) until a chunk reaches the done state. */
static void WaitChunkDone(const TTestChunk& tChunk)
{
    for (int nWait = 0; nWait < 200; ++nWait)
    {
        if (tChunk.bDone.load() != FALSE)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

/** @brief Verify a buffer matches the deterministic pattern. */
static BOOL32 BufferMatches(const std::vector<u8>& vecBuffer,
                            u64 dwStart, u64 dwLen)
{
    for (u64 dwIndex = 0; dwIndex < dwLen; ++dwIndex)
    {
        if (vecBuffer[static_cast<size_t>(dwStart + dwIndex)] !=
            PatternByte(dwStart + dwIndex))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/** @brief Test: one task's chunks all run concurrently on the lanes. */
static void TestMultiParallelChunks(CTestReport& cReport)
{
    cReport.BeginCase("curlmulti: parallel chunk transfers");
    const u32 dwFileSize = 4u * 1024u * 1024u;  /* 4 MiB */
    CLocalHttpServer srv;
    BURST_EXPECT_TRUE(cReport, srv.Start(dwFileSize, 1000u) != FALSE);

    std::vector<u8> vecFile(dwFileSize, 0u);
    CCurlMultiEngine cEngine(4u);
    const u32 dwChunks = 8u;
    std::vector<TTestChunk> vecChunks(dwChunks);
    std::vector<std::string> vecRanges(dwChunks);

    for (u32 dwIndex = 0; dwIndex < dwChunks; ++dwIndex)
    {
        const u64 dwStart =
            static_cast<u64>(dwFileSize) * dwIndex / dwChunks;
        u64 dwEnd =
            static_cast<u64>(dwFileSize) * (dwIndex + 1u) / dwChunks - 1u;
        if (dwIndex + 1u == dwChunks)
        {
            dwEnd = static_cast<u64>(dwFileSize) - 1u;
        }
        TTestChunk& tChunk = vecChunks[dwIndex];
        tChunk.pBuffer = &vecFile;
        tChunk.dwStart = dwStart;
        tChunk.dwEnd = dwEnd;
        char szRange[64];
        snprintf(szRange, sizeof(szRange), "%llu-%llu",
                 static_cast<unsigned long long>(dwStart),
                 static_cast<unsigned long long>(dwEnd));
        vecRanges[dwIndex] = szRange;

        TChunkJob tJob;
        tJob.pUserData = &tChunk;
        tJob.dwLane = kChunkLaneUnset;
        tJob.pCancelFlag = nullptr;
        tJob.fnCreateEasy = [&srv, &vecChunks, &vecRanges, dwIndex]()
            -> CURL* {
            return CreateTestEasy(srv.Url(), &vecChunks[dwIndex],
                                  vecRanges[dwIndex]);
        };
        tJob.fnDone = [&tChunk](CURLcode code, long lHttp) {
            tChunk.code.store(code);
            tChunk.lHttp.store(lHttp);
            tChunk.bDone.store(TRUE);
        };
        BURST_EXPECT_TRUE(cReport, cEngine.SubmitChunk(tJob) != FALSE);
    }

    for (u32 dwIndex = 0; dwIndex < dwChunks; ++dwIndex)
    {
        WaitChunkDone(vecChunks[dwIndex]);
        BURST_EXPECT_TRUE(cReport, vecChunks[dwIndex].bDone.load() != FALSE);
        BURST_EXPECT_TRUE(cReport,
                          vecChunks[dwIndex].code.load() == CURLE_OK);
        BURST_EXPECT_TRUE(cReport, vecChunks[dwIndex].lHttp.load() == 206L);
        BURST_EXPECT_TRUE(
            cReport,
            vecChunks[dwIndex].dwOffset.load() ==
                vecChunks[dwIndex].dwEnd - vecChunks[dwIndex].dwStart + 1u);
    }
    BURST_EXPECT_TRUE(cReport,
                      BufferMatches(vecFile, 0u, dwFileSize) != FALSE);
    /* All 8 chunks must be connected at once (not limited by the 4 lanes). */
    BURST_EXPECT_TRUE(cReport, srv.MaxConcurrent() >= dwChunks);
    BURST_EXPECT_TRUE(cReport, cEngine.ActiveCount() == 0u);
    srv.Stop();
}

/** @brief Test: two tasks submit concurrently and share the same engine. */
static void TestMultiTaskConcurrency(CTestReport& cReport)
{
    cReport.BeginCase("curlmulti: concurrent tasks share the engine");
    const u32 dwFileSize = 1024u * 1024u;  /* 1 MiB per task */
    CLocalHttpServer srv;
    BURST_EXPECT_TRUE(cReport, srv.Start(dwFileSize, 800u) != FALSE);

    std::vector<u8> vecFileA(dwFileSize, 0u);
    std::vector<u8> vecFileB(dwFileSize, 0u);
    CCurlMultiEngine cEngine(2u);  /* fewer lanes than total chunks */
    const u32 dwChunksPerTask = 4u;

    typedef struct tagTaskCtx
    {
        std::vector<u8>* pBuffer;
        std::vector<TTestChunk> vecChunks;
        std::vector<std::string> vecRanges;
        CCurlMultiEngine* pEngine;
        CLocalHttpServer* pServer;
    } TTaskCtx;

    TTaskCtx tCtxA;
    tCtxA.pBuffer = &vecFileA;
    tCtxA.pEngine = &cEngine;
    tCtxA.pServer = &srv;
    TTaskCtx tCtxB;
    tCtxB.pBuffer = &vecFileB;
    tCtxB.pEngine = &cEngine;
    tCtxB.pServer = &srv;

    auto fnFillTask = [dwFileSize, dwChunksPerTask](TTaskCtx& tCtx) {
        /* TTestChunk holds atomics (not movable), so size upfront. */
        tCtx.vecChunks = std::vector<TTestChunk>(dwChunksPerTask);
        tCtx.vecRanges = std::vector<std::string>(dwChunksPerTask);
        for (u32 dwIndex = 0; dwIndex < dwChunksPerTask; ++dwIndex)
        {
            const u64 dwStart =
                static_cast<u64>(dwFileSize) * dwIndex / dwChunksPerTask;
            u64 dwEnd =
                static_cast<u64>(dwFileSize) * (dwIndex + 1u) /
                    dwChunksPerTask -
                1u;
            if (dwIndex + 1u == dwChunksPerTask)
            {
                dwEnd = static_cast<u64>(dwFileSize) - 1u;
            }
            TTestChunk& tChunk = tCtx.vecChunks[dwIndex];
            tChunk.pBuffer = tCtx.pBuffer;
            tChunk.dwStart = dwStart;
            tChunk.dwEnd = dwEnd;
            char szRange[64];
            snprintf(szRange, sizeof(szRange), "%llu-%llu",
                     static_cast<unsigned long long>(dwStart),
                     static_cast<unsigned long long>(dwEnd));
            tCtx.vecRanges[dwIndex] = szRange;
        }
    };
    fnFillTask(tCtxA);
    fnFillTask(tCtxB);

    auto fnSubmitTask = [](TTaskCtx& tCtx) {
        for (u32 dwIndex = 0; dwIndex < tCtx.vecChunks.size(); ++dwIndex)
        {
            TTestChunk& tChunk = tCtx.vecChunks[dwIndex];
            TChunkJob tJob;
            tJob.pUserData = &tChunk;
            tJob.dwLane = kChunkLaneUnset;
            tJob.pCancelFlag = nullptr;
            tJob.fnCreateEasy = [&tCtx, dwIndex]() -> CURL* {
                return CreateTestEasy(tCtx.pServer->Url(),
                                      &tCtx.vecChunks[dwIndex],
                                      tCtx.vecRanges[dwIndex]);
            };
            tJob.fnDone = [&tChunk](CURLcode code, long lHttp) {
                tChunk.code.store(code);
                tChunk.lHttp.store(lHttp);
                tChunk.bDone.store(TRUE);
            };
            tCtx.pEngine->SubmitChunk(tJob);
        }
    };

    std::thread thA(fnSubmitTask, std::ref(tCtxA));
    std::thread thB(fnSubmitTask, std::ref(tCtxB));
    thA.join();
    thB.join();

    auto fnVerifyTask = [&cReport](TTaskCtx& tCtx, u32 dwFileSizeArg) {
        for (u32 dwIndex = 0; dwIndex < tCtx.vecChunks.size(); ++dwIndex)
        {
            TTestChunk& tChunk = tCtx.vecChunks[dwIndex];
            WaitChunkDone(tChunk);
            BURST_EXPECT_TRUE(cReport, tChunk.bDone.load() != FALSE);
            BURST_EXPECT_TRUE(cReport, tChunk.code.load() == CURLE_OK);
            BURST_EXPECT_TRUE(cReport, tChunk.lHttp.load() == 206L);
            BURST_EXPECT_TRUE(
                cReport,
                tChunk.dwOffset.load() ==
                    tChunk.dwEnd - tChunk.dwStart + 1u);
        }
        BURST_EXPECT_TRUE(
            cReport,
            BufferMatches(*tCtx.pBuffer, 0u, dwFileSizeArg) != FALSE);
    };
    fnVerifyTask(tCtxA, dwFileSize);
    fnVerifyTask(tCtxB, dwFileSize);
    BURST_EXPECT_TRUE(cReport, cEngine.ActiveCount() == 0u);
    srv.Stop();
}

/** @brief Test: a canceled transfer completes promptly. */
static void TestMultiCancel(CTestReport& cReport)
{
    cReport.BeginCase("curlmulti: cancellation aborts a stuck transfer");
    CLocalHttpServer srv;
    BURST_EXPECT_TRUE(cReport, srv.Start(1024u * 1024u, 0u) != FALSE);
    CCurlMultiEngine cEngine(1u);

    std::vector<u8> vecFile(1024u * 1024u, 0u);
    TTestChunk tChunk;
    tChunk.pBuffer = &vecFile;
    tChunk.dwStart = 0u;
    tChunk.dwEnd = 1024u * 1024u - 1u;
    std::atomic<bool> bCancel{false};

    /* Point at /slow: the endpoint is its own path on the same port. */
    std::string strSlowUrl = srv.Url();
    strSlowUrl.replace(strSlowUrl.find("/file"), 5u, "/slow");

    TChunkJob tJob;
    tJob.pUserData = &tChunk;
    tJob.dwLane = kChunkLaneUnset;
    tJob.pCancelFlag = &bCancel;
    tJob.fnCreateEasy = [&tChunk, &strSlowUrl]() -> CURL* {
        return CreateTestEasy(strSlowUrl, &tChunk, "0-1048575");
    };
    tJob.fnDone = [&tChunk](CURLcode code, long lHttp) {
        tChunk.code.store(code);
        tChunk.lHttp.store(lHttp);
        tChunk.bDone.store(TRUE);
    };
    BURST_EXPECT_TRUE(cReport, cEngine.SubmitChunk(tJob) != FALSE);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bCancel.store(true);
    WaitChunkDone(tChunk);
    BURST_EXPECT_TRUE(cReport, tChunk.bDone.load() != FALSE);
    BURST_EXPECT_TRUE(
        cReport, tChunk.code.load() == CURLE_ABORTED_BY_CALLBACK);
    BURST_EXPECT_TRUE(cReport, cEngine.ActiveCount() == 0u);
    srv.Stop();
}

/** @brief Test: a completed attempt can be re-submitted after a delay. */
static void TestMultiRetryDelayed(CTestReport& cReport)
{
    cReport.BeginCase("curlmulti: delayed retry re-submission");
    CLocalHttpServer srv;
    BURST_EXPECT_TRUE(cReport, srv.Start(256u * 1024u, 0u) != FALSE);
    CCurlMultiEngine cEngine(1u);

    std::vector<u8> vecFile(256u * 1024u, 0u);
    TTestChunk tChunk;
    tChunk.pBuffer = &vecFile;
    tChunk.dwStart = 0u;
    tChunk.dwEnd = 256u * 1024u - 1u;
    std::shared_ptr<std::string> pUrl =
        std::make_shared<std::string>(srv.FailUrl());
    std::atomic<u32> dwDoneCalls{0u};
    const std::function<CURL*()> fnCreate = [pUrl, &tChunk]() -> CURL* {
        return CreateTestEasy(*pUrl, &tChunk, "0-262143");
    };

    TChunkJob tJob;
    tJob.pUserData = &tChunk;
    tJob.dwLane = kChunkLaneUnset;
    tJob.pCancelFlag = nullptr;
    tJob.fnCreateEasy = fnCreate;
    std::shared_ptr<std::function<void(CURLcode, long)> > pfnDone =
        std::make_shared<std::function<void(CURLcode, long)> >();
    *pfnDone = [&cEngine, &srv, pUrl, &tChunk, &tJob, &dwDoneCalls,
                pfnDone, fnCreate](CURLcode code, long lHttp) {
        const u32 nCalls = dwDoneCalls.fetch_add(1u) + 1u;
        if ((nCalls == 1u) && (lHttp == 500L))
        {
            *pUrl = srv.Url();  /* retry against the working endpoint */
            TChunkJob tRetry;
            tRetry.pUserData = &tChunk;
            tRetry.dwLane = tJob.dwLane;  /* same lane */
            tRetry.pCancelFlag = nullptr;
            tRetry.fnCreateEasy = fnCreate;
            tRetry.fnDone = *pfnDone;
            cEngine.SubmitChunkDelayed(tRetry, 100u);
        }
        else
        {
            tChunk.code.store(code);
            tChunk.lHttp.store(lHttp);
            tChunk.bDone.store(TRUE);
        }
    };
    tJob.fnDone = *pfnDone;
    BURST_EXPECT_TRUE(cReport, cEngine.SubmitChunk(tJob) != FALSE);

    WaitChunkDone(tChunk);
    BURST_EXPECT_TRUE(cReport, tChunk.bDone.load() != FALSE);
    BURST_EXPECT_TRUE(cReport, tChunk.code.load() == CURLE_OK);
    BURST_EXPECT_TRUE(cReport, tChunk.lHttp.load() == 206L);
    BURST_EXPECT_TRUE(cReport,
                      tChunk.dwOffset.load() ==
                          tChunk.dwEnd - tChunk.dwStart + 1u);
    BURST_EXPECT_TRUE(cReport, dwDoneCalls.load() == 2u);
    BURST_EXPECT_TRUE(cReport, cEngine.ActiveCount() == 0u);
    srv.Stop();
}

/** @brief Test: Shutdown cancels everything and releases waiters. */
static void TestMultiShutdown(CTestReport& cReport)
{
    cReport.BeginCase("curlmulti: shutdown releases waiters");
    CLocalHttpServer srv;
    BURST_EXPECT_TRUE(cReport, srv.Start(1024u * 1024u, 0u) != FALSE);
    {
        CCurlMultiEngine cEngine(2u);
        std::vector<u8> vecFile(1024u * 1024u, 0u);
        TTestChunk tChunk;
        tChunk.pBuffer = &vecFile;
        tChunk.dwStart = 0u;
        tChunk.dwEnd = 1024u * 1024u - 1u;
        std::string strSlow = srv.Url();
        strSlow.replace(strSlow.find("/file"), 5u, "/slow");

        TChunkJob tJob;
        tJob.pUserData = &tChunk;
        tJob.dwLane = kChunkLaneUnset;
        tJob.pCancelFlag = nullptr;
        tJob.fnCreateEasy = [&tChunk, &strSlow]() -> CURL* {
            return CreateTestEasy(strSlow, &tChunk, "0-1048575");
        };
        tJob.fnDone = [&tChunk](CURLcode code, long lHttp) {
            tChunk.code.store(code);
            tChunk.lHttp.store(lHttp);
            tChunk.bDone.store(TRUE);
        };
        BURST_EXPECT_TRUE(cReport, cEngine.SubmitChunk(tJob) != FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        cEngine.Shutdown();
        BURST_EXPECT_TRUE(cReport, tChunk.bDone.load() != FALSE);
        BURST_EXPECT_TRUE(
            cReport, tChunk.code.load() == CURLE_ABORTED_BY_CALLBACK);
        BURST_EXPECT_TRUE(cReport, cEngine.ActiveCount() == 0u);
    }
    srv.Stop();
}

}  // namespace

/** @brief Run all curl_multi engine tests. */
void RunCurlMultiTests(CTestReport& cReport)
{
    TestMultiParallelChunks(cReport);
    TestMultiTaskConcurrency(cReport);
    TestMultiCancel(cReport);
    TestMultiRetryDelayed(cReport);
    TestMultiShutdown(cReport);
}
