#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

#include "network_http.h"
#include "sys_agent_version.h"

#define NETWORK_HTTP_URL_MAX 1024
#define NETWORK_HTTP_RESPONSE_MAX (32 * 1024)
#define NETWORK_HTTP_TIMEOUT_MS 8000
#define NETWORK_HTTP_THREAD_STACK_SIZE 0x10000
#define NETWORK_HTTP_DEFAULT_PUBLIC_IP_URL "https://ifconfig.co/json"

typedef struct {
    bool tls;
    char host[256];
    char port[8];
    char path[768];
} HttpUrl;

typedef struct {
    Mutex mutex;
    CondVar condvar;
    Thread thread;
    bool started;
    bool pending;
    int clientFd;
    char url[NETWORK_HTTP_URL_MAX];
} NetworkHttpState;

typedef enum {
    HttpReadOk = 0,
    HttpReadFailed,
    HttpReadTimeout,
    HttpReadTooLarge,
} HttpReadResult;

static NetworkHttpState httpState;

static bool sendAll(int fd, const void* data, size_t size)
{
    const u8* bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = send(fd, bytes + offset, size - offset, 0);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        offset += (size_t)written;
    }
    return true;
}

static void sendError(int fd, const char* code, const char* host)
{
    char response[512];
    int length;
    if (host && *host) {
        length = snprintf(response, sizeof(response),
            "ERR code=%s host=%s errno=%d\n", code, host, errno);
    } else {
        length = snprintf(response, sizeof(response),
            "ERR code=%s errno=%d\n", code, errno);
    }
    if (length > 0)
        sendAll(fd, response, (size_t)length);
}

static bool parseHttpUrl(const char* input, HttpUrl* out)
{
    if (!input || !*input) return false;
    memset(out, 0, sizeof(*out));
    const char* cursor = input;
    if (!strncmp(cursor, "https://", 8)) {
        out->tls = true;
        cursor += 8;
    } else if (!strncmp(cursor, "http://", 7)) {
        out->tls = false;
        cursor += 7;
    } else {
        out->tls = true;
    }
    if (!*cursor || strchr(cursor, ' ') || strchr(cursor, '\t')
        || strchr(cursor, '\r') || strchr(cursor, '\n')) {
        return false;
    }

    const char* authorityEnd = strchr(cursor, '/');
    size_t authorityLength = authorityEnd ? (size_t)(authorityEnd - cursor) : strlen(cursor);
    const char* portSeparator = memchr(cursor, ':', authorityLength);
    size_t hostLength = portSeparator ? (size_t)(portSeparator - cursor) : authorityLength;
    if (hostLength == 0 || hostLength >= sizeof(out->host)) return false;
    memcpy(out->host, cursor, hostLength);
    out->host[hostLength] = 0;

    if (portSeparator) {
        size_t portLength = authorityLength - hostLength - 1;
        if (portLength == 0 || portLength >= sizeof(out->port)) return false;
        memcpy(out->port, portSeparator + 1, portLength);
        out->port[portLength] = 0;
    } else {
        snprintf(out->port, sizeof(out->port), "%s", out->tls ? "443" : "80");
    }
    int pathLength = snprintf(out->path, sizeof(out->path), "%s",
        authorityEnd ? authorityEnd : "/");
    return pathLength > 0 && (size_t)pathLength < sizeof(out->path);
}

static bool connectSocketWithTimeout(int socketFd,
    const struct sockaddr* address, socklen_t addressLength)
{
    int flags = fcntl(socketFd, F_GETFL, 0);
    if (flags < 0 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) != 0)
        return connect(socketFd, address, addressLength) == 0;

    bool connected = false;
    int rc = connect(socketFd, address, addressLength);
    if (rc == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pollFd = {
            .fd = socketFd,
            .events = POLLOUT,
        };
        int ready = poll(&pollFd, 1, NETWORK_HTTP_TIMEOUT_MS);
        if (ready > 0) {
            int socketError = 0;
            socklen_t errorLength = sizeof(socketError);
            if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR,
                    &socketError, &errorLength) == 0) {
                connected = socketError == 0;
                if (!connected) errno = socketError;
            }
        } else if (ready == 0) {
            errno = ETIMEDOUT;
        }
    }
    return connected && fcntl(socketFd, F_SETFL, flags) == 0;
}

static int connectHttpHost(const char* host, const char* port)
{
    struct addrinfo hints = {0};
    struct addrinfo* addresses = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    errno = 0;
    int rc = getaddrinfo(host, port, &hints, &addresses);
    if (rc != 0 || !addresses) return -1;

    int socketFd = -1;
    for (struct addrinfo* address = addresses; address; address = address->ai_next) {
        socketFd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketFd < 0) continue;
        if (connectSocketWithTimeout(socketFd,
                address->ai_addr, address->ai_addrlen)) {
            break;
        }
        close(socketFd);
        socketFd = -1;
    }
    freeaddrinfo(addresses);
    return socketFd;
}

static bool sendTlsOrSocket(int socketFd, SslConnection* ssl,
    const void* data, size_t size)
{
    if (!ssl)
        return sendAll(socketFd, data, size);

    const u8* bytes = data;
    size_t offset = 0;
    while (offset < size) {
        u32 written = 0;
        Result rc = sslConnectionWrite(ssl, bytes + offset,
            (u32)(size - offset), &written);
        if (R_FAILED(rc) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

static bool startsWithIgnoreCase(const char* value, const char* prefix)
{
    size_t length = strlen(prefix);
    return !strncasecmp(value, prefix, length);
}

static bool httpResponseComplete(const char* response, size_t size)
{
    const char* separator = strstr(response, "\r\n\r\n");
    if (!separator)
        return false;

    size_t headerSize = (size_t)(separator - response) + 4;
    const char* cursor = response;
    while (cursor < separator) {
        const char* lineEnd = strstr(cursor, "\r\n");
        if (!lineEnd || lineEnd > separator)
            break;
        if (startsWithIgnoreCase(cursor, "content-length:")) {
            const char* value = cursor + strlen("content-length:");
            while (value < lineEnd && (*value == ' ' || *value == '\t')) value++;
            char* parsedEnd = NULL;
            errno = 0;
            unsigned long contentLength = strtoul(value, &parsedEnd, 10);
            if (errno == 0 && parsedEnd != value && parsedEnd <= lineEnd) {
                size_t expected = headerSize + (size_t)contentLength;
                return size >= expected;
            }
        }
        cursor = lineEnd + 2;
    }

    int status = size >= 12 && !strncmp(response, "HTTP/", 5)
        ? atoi(response + 9) : 0;
    return status == 204 || status == 304;
}

static HttpReadResult readHttpResponse(int socketFd, SslConnection* ssl,
    char* buffer, size_t capacity, size_t* outSize)
{
    *outSize = 0;
    while (*outSize + 1 < capacity) {
        buffer[*outSize] = 0;
        if (httpResponseComplete(buffer, *outSize))
            return HttpReadOk;

        u32 readSize = 0;
        if (ssl) {
            Result rc = sslConnectionRead(ssl, buffer + *outSize,
                (u32)(capacity - 1 - *outSize), &readSize);
            if (R_FAILED(rc))
                return HttpReadFailed;
        } else {
            struct pollfd pollFd = {
                .fd = socketFd,
                .events = POLLIN,
            };
            int ready = poll(&pollFd, 1, NETWORK_HTTP_TIMEOUT_MS);
            if (ready == 0)
                return HttpReadTimeout;
            if (ready < 0) {
                if (errno == EINTR) continue;
                return HttpReadFailed;
            }
            ssize_t received = recv(socketFd, buffer + *outSize,
                capacity - 1 - *outSize, 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                return HttpReadFailed;
            }
            readSize = (u32)received;
        }
        if (readSize == 0) {
            buffer[*outSize] = 0;
            return HttpReadOk;
        }
        *outSize += readSize;
    }
    buffer[*outSize] = 0;
    return *outSize + 1 >= capacity ? HttpReadTooLarge : HttpReadOk;
}

static void sendHttpSuccess(int fd, const char* url, int status,
    const char* body, size_t bodySize)
{
    char header[1536];
    int headerLength = snprintf(header, sizeof(header),
        "OK status=%d url=%s bodyLen=%zu body=", status, url, bodySize);
    if (headerLength <= 0 || (size_t)headerLength >= sizeof(header)
        || !sendAll(fd, header, (size_t)headerLength)) {
        return;
    }

    static const char hexDigits[] = "0123456789ABCDEF";
    char hex[512];
    size_t offset = 0;
    while (offset < bodySize) {
        size_t chunk = bodySize - offset;
        if (chunk > sizeof(hex) / 2) chunk = sizeof(hex) / 2;
        for (size_t i = 0; i < chunk; i++) {
            u8 value = (u8)body[offset + i];
            hex[i * 2] = hexDigits[value >> 4];
            hex[i * 2 + 1] = hexDigits[value & 0xF];
        }
        if (!sendAll(fd, hex, chunk * 2))
            return;
        offset += chunk;
    }
    sendAll(fd, "\n", 1);
}

static void performHttpRequest(int clientFd, const char* url)
{
    HttpUrl parsed;
    errno = 0;
    if (!parseHttpUrl(url, &parsed)) {
        sendError(clientFd, "INVALID_URL", NULL);
        return;
    }

    int socketFd = connectHttpHost(parsed.host, parsed.port);
    if (socketFd < 0) {
        sendError(clientFd, "CONNECT_FAILED", parsed.host);
        return;
    }

    char request[1536];
    int requestLength = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: sys-agent/" SYS_AGENT_VERSION "\r\n"
        "Accept: application/json, text/plain, */*\r\n"
        "Connection: close\r\n\r\n",
        parsed.path, parsed.host);
    if (requestLength <= 0 || (size_t)requestLength >= sizeof(request)) {
        close(socketFd);
        sendError(clientFd, "INVALID_URL", parsed.host);
        return;
    }

    SslContext context;
    SslConnection connection;
    SslConnection* ssl = NULL;
    bool sslInitialized = false;
    bool contextCreated = false;
    bool connectionCreated = false;
    const char* stageError = NULL;
    Result stageResult = 0;

    if (parsed.tls) {
        Result rc = sslInitialize(1);
        if (R_SUCCEEDED(rc)) {
            sslInitialized = true;
            rc = sslCreateContext(&context, SslVersion_Auto);
        }
        if (R_SUCCEEDED(rc)) {
            contextCreated = true;
            rc = sslContextCreateConnection(&context, &connection);
        }
        if (R_SUCCEEDED(rc)) {
            connectionCreated = true;
            rc = sslConnectionSetOption(&connection,
                SslOptionType_DoNotCloseSocket, true);
        }
        if (R_SUCCEEDED(rc)) {
            errno = 0;
            int retainedSocket = socketSslConnectionSetSocketDescriptor(&connection, socketFd);
            if (retainedSocket < 0 && errno != ENOENT)
                rc = socketGetLastResult();
        }
        if (R_SUCCEEDED(rc)) {
            rc = sslConnectionSetHostName(&connection, parsed.host, strlen(parsed.host));
        }
        if (R_SUCCEEDED(rc)) {
            sslConnectionSetIoTimeout(&connection, NETWORK_HTTP_TIMEOUT_MS);
            u32 certSize = 0, certCount = 0;
            rc = sslConnectionDoHandshake(&connection, &certSize, &certCount, NULL, 0);
        }
        if (R_FAILED(rc)) {
            if (!sslInitialized) stageError = "SSL_INIT_FAILED";
            else if (!connectionCreated) stageError = "SSL_CONTEXT_FAILED";
            else stageError = "SSL_HANDSHAKE_FAILED";
            stageResult = rc;
        } else {
            ssl = &connection;
        }
    }

    char* response = NULL;
    size_t responseSize = 0;
    bool sent = false;
    HttpReadResult readResult = HttpReadFailed;
    if (!stageError) {
        response = malloc(NETWORK_HTTP_RESPONSE_MAX);
        sent = response && sendTlsOrSocket(socketFd, ssl,
            request, (size_t)requestLength);
        if (sent)
            readResult = readHttpResponse(socketFd, ssl, response,
                NETWORK_HTTP_RESPONSE_MAX, &responseSize);
    }

    /* Every request is one-shot. Close TLS cleanly first, then the underlying socket. */
    if (connectionCreated) sslConnectionClose(&connection);
    if (contextCreated) sslContextClose(&context);
    if (sslInitialized) sslExit();
    close(socketFd);

    if (stageError) {
        char responseText[256];
        int length = snprintf(responseText, sizeof(responseText),
            "ERR code=%s result=0x%X host=%s\n",
            stageError, stageResult, parsed.host);
        if (length > 0)
            sendAll(clientFd, responseText, (size_t)length);
        free(response);
        return;
    }
    if (!response) {
        sendError(clientFd, "NO_MEMORY", parsed.host);
        return;
    }
    if (!sent) {
        free(response);
        sendError(clientFd, "HTTP_REQUEST_FAILED", parsed.host);
        return;
    }
    if (readResult == HttpReadTimeout) {
        free(response);
        sendError(clientFd, "HTTP_TIMEOUT", parsed.host);
        return;
    }
    if (readResult == HttpReadTooLarge) {
        free(response);
        sendError(clientFd, "HTTP_RESPONSE_TOO_LARGE", parsed.host);
        return;
    }
    if (readResult != HttpReadOk) {
        free(response);
        sendError(clientFd, "HTTP_READ_FAILED", parsed.host);
        return;
    }

    int status = 0;
    if (responseSize >= 12 && !strncmp(response, "HTTP/", 5))
        status = atoi(response + 9);
    char* body = strstr(response, "\r\n\r\n");
    if (status == 0 || !body) {
        free(response);
        sendError(clientFd, "INVALID_HTTP_RESPONSE", parsed.host);
        return;
    }
    body += 4;
    size_t bodySize = responseSize - (size_t)(body - response);
    sendHttpSuccess(clientFd, url, status, body, bodySize);
    free(response);
}

static void networkHttpWorker(void* unused)
{
    (void)unused;
    char url[NETWORK_HTTP_URL_MAX];

    mutexLock(&httpState.mutex);
    while (true) {
        while (!httpState.pending)
            condvarWait(&httpState.condvar, &httpState.mutex);

        int clientFd = httpState.clientFd;
        snprintf(url, sizeof(url), "%s", httpState.url);
        mutexUnlock(&httpState.mutex);

        performHttpRequest(clientFd, url);
        close(clientFd);

        mutexLock(&httpState.mutex);
        httpState.pending = false;
    }
}

Result networkHttpInitialize(void)
{
    if (httpState.started)
        return 0;

    mutexInit(&httpState.mutex);
    condvarInit(&httpState.condvar);
    Result rc = threadCreate(&httpState.thread, networkHttpWorker, NULL, NULL,
        NETWORK_HTTP_THREAD_STACK_SIZE, 0x2C, -2);
    if (R_FAILED(rc))
        return rc;
    rc = threadStart(&httpState.thread);
    if (R_FAILED(rc)) {
        threadClose(&httpState.thread);
        return rc;
    }
    httpState.started = true;
    return 0;
}

NetworkHttpSubmitResult networkHttpSubmit(bool publicIpDefault, const char* url)
{
    if (!httpState.started)
        return NetworkHttpUnavailable;

    const char* target = url;
    if (publicIpDefault && (!target || !*target))
        target = NETWORK_HTTP_DEFAULT_PUBLIC_IP_URL;
    if (!target || !*target || strlen(target) >= sizeof(httpState.url))
        return NetworkHttpInvalidUrl;

    mutexLock(&httpState.mutex);
    if (httpState.pending) {
        mutexUnlock(&httpState.mutex);
        return NetworkHttpBusy;
    }

    int clientFd = dup(STDOUT_FILENO);
    if (clientFd < 0) {
        mutexUnlock(&httpState.mutex);
        return NetworkHttpUnavailable;
    }
    snprintf(httpState.url, sizeof(httpState.url), "%s", target);
    httpState.clientFd = clientFd;
    httpState.pending = true;
    condvarWakeOne(&httpState.condvar);
    mutexUnlock(&httpState.mutex);
    return NetworkHttpSubmitted;
}
