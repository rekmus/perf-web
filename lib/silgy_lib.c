/* --------------------------------------------------------------------------
   Silgy Web App Engine
   Jurek Muszynski
   silgy.com
   Some parts are Public Domain from other authors, see below
-----------------------------------------------------------------------------
   General purpose library
-------------------------------------------------------------------------- */

#include <silgy.h>

#ifdef ICONV
#include <iconv.h>
#endif


#define RANDOM_NUMBERS 1024*64



/* globals */

int         G_logLevel=3;               /* log level -- 'info' by default */
int         G_logToStdout=0;            /* log to stdout */
int         G_logCombined=0;            /* standard log format */
char        G_appdir[256]="..";         /* application root dir */
int         G_RESTTimeout=CALL_REST_DEFAULT_TIMEOUT;
int         G_test=0;                   /* test run */
int         G_pid=0;                    /* pid */
time_t      G_now=0;                    /* current time (GMT) */
struct tm   *G_ptm={0};                 /* human readable current time */
char        G_dt[20]="";                /* datetime for database or log (YYYY-MM-DD hh:mm:ss) */
char        G_tmp[TMP_BUFSIZE];         /* temporary string buffer */
messages_t  G_messages[MAX_MESSAGES]={0};
int         G_next_message=0;
#ifdef HTTPS
bool        G_ssl_lib_initialized=0;
#endif
char        *G_shm_segptr=NULL;         /* SHM pointer */

long        G_rest_req=0;               /* REST calls counter */
double      G_rest_elapsed=0;           /* REST calls elapsed for calculating average */
double      G_rest_average=0;           /* REST calls average elapsed */
int         G_rest_status;              /* last REST call response status */
char        G_rest_content_type[MAX_VALUE_LEN+1];


/* locals */

static char *M_conf=NULL;               /* config file content */
static FILE *M_log_fd=NULL;             /* log file handle */

static char M_df=0;                     /* date format */
static char M_tsep=' ';                 /* thousand separator */
static char M_dsep='.';                 /* decimal separator */

static int  M_shmid;                    /* SHM id */

static rest_header_t M_rest_headers[REST_MAX_HEADERS];
static int M_rest_headers_cnt=0;
#ifdef _WIN32   /* Windows */
static SOCKET M_rest_sock;
#else
static int M_rest_sock;
#endif  /* _WIN32 */
#ifdef HTTPS
static SSL_CTX *M_ssl_ctx=NULL;
static SSL *M_rest_ssl=NULL;
#else
static void *M_rest_ssl=NULL;    /* dummy */
#endif  /* HTTPS */

static bool M_rest_proxy=FALSE;

static unsigned char M_random_numbers[RANDOM_NUMBERS];
static char M_random_initialized=0;


static void seed_rand(void);
static void minify_1(char *dest, const char *src, int len);
static int  minify_2(char *dest, const char *src);
static void get_byteorder32(void);
static void get_byteorder64(void);


/* --------------------------------------------------------------------------
   Library init
-------------------------------------------------------------------------- */
void silgy_lib_init()
{
    DBG("silgy_lib_init");
    /* G_pid */
    G_pid = getpid();
    /* G_appdir */
    lib_get_app_dir();
    /* time globals */
    lib_update_time_globals();
    /* log file fd */
    M_log_fd = stdout;
}


/* --------------------------------------------------------------------------
   Add error message
-------------------------------------------------------------------------- */
void silgy_add_message(int code, const char *lang, const char *message, ...)
{
    if ( G_next_message >= MAX_MESSAGES )
    {
        ERR("MAX_MESSAGES (%d) has been reached", MAX_MESSAGES);
        return;
    }

    va_list plist;
    char    buffer[MAX_MSG_LEN+1];

    /* compile message with arguments into buffer */

    va_start(plist, message);
    vsprintf(buffer, message, plist);
    va_end(plist);

    G_messages[G_next_message].code = code;
    if ( lang )
        strcpy(G_messages[G_next_message].lang, upper(lang));
    strcpy(G_messages[G_next_message].message, buffer);

    ++G_next_message;
}


/* --------------------------------------------------------------------------
   Get error description for user
-------------------------------------------------------------------------- */
char *silgy_message(int code)
{
    int i;
    for ( i=0; i<G_next_message; ++i )
        if ( G_messages[i].code == code )
            return G_messages[i].message;

static char unknown[128];
    sprintf(unknown, "Unknown code: %d", code);
    return unknown;
}


/* --------------------------------------------------------------------------
   Get error description for user
   Pick the user agent language if possible
-------------------------------------------------------------------------- */
char *silgy_message_lang(int ci, int code)
{
#ifndef SILGY_WATCHER

    int i;
    for ( i=0; i<G_next_message; ++i )
        if ( G_messages[i].code == code && 0==strcmp(G_messages[i].lang, conn[ci].lang) )
            return G_messages[i].message;

    /* fallback */

    return silgy_message(code);

#endif  /* SILGY_WATCHER */
}


/* --------------------------------------------------------------------------
   URI encoding
---------------------------------------------------------------------------*/
char *urlencode(const char *src)
{
static char     dest[4096];
    int         i, j=0;
    const char  *hex="0123456789ABCDEF";

    for ( i=0; src[i] && j<4092; ++i )
    {
        if ( isalnum(src[i]) )
        {
            dest[j++] = src[i];
        }
        else
        {
            dest[j++] = '%';
            dest[j++] = hex[src[i] >> 4];
            dest[j++] = hex[src[i] & 15];
        }
    }

    dest[j] = EOS;

    return dest;
}


/* --------------------------------------------------------------------------
   Open database connection
-------------------------------------------------------------------------- */
bool lib_open_db()
{
#ifdef DBMYSQL
    if ( !G_dbName[0] )
    {
        ERR("dbName parameter is required in silgy.conf");
        return FALSE;
    }

    if ( NULL == (G_dbconn=mysql_init(NULL)) )
    {
        ERR("Error %u: %s", mysql_errno(G_dbconn), mysql_error(G_dbconn));
        return FALSE;
    }

#ifdef DBMYSQLRECONNECT
    my_bool reconnect=1;
    mysql_options(G_dbconn, MYSQL_OPT_RECONNECT, &reconnect);
#endif

//    unsigned long max_packet=33554432;  /* 32 MB */
//    mysql_options(G_dbconn, MYSQL_OPT_MAX_ALLOWED_PACKET, &max_packet);

    if ( NULL == mysql_real_connect(G_dbconn, G_dbHost[0]?G_dbHost:NULL, G_dbUser, G_dbPassword, G_dbName, G_dbPort, NULL, 0) )
    {
        ERR("Error %u: %s", mysql_errno(G_dbconn), mysql_error(G_dbconn));
        return FALSE;
    }
#endif
    return TRUE;
}


/* --------------------------------------------------------------------------
   Close database connection
-------------------------------------------------------------------------- */
void lib_close_db()
{
#ifdef DBMYSQL
    if ( G_dbconn )
        mysql_close(G_dbconn);
#endif
}


/* --------------------------------------------------------------------------
   Return TRUE if file exists and it's readable
-------------------------------------------------------------------------- */
bool lib_file_exists(const char *fname)
{
    FILE *f=NULL;

    if ( NULL != (f=fopen(fname, "r")) )
    {
        fclose(f);
        return TRUE;
    }

    return FALSE;
}


/* --------------------------------------------------------------------------
   Get the last part of path
-------------------------------------------------------------------------- */
void lib_get_exec_name(char *dst, const char *path)
{
    const char *p=path;
    const char *pd=NULL;

    while ( *p )
    {
#ifdef _WIN32
        if ( *p == '\\' )
#else
        if ( *p == '/' )
#endif
        {
            if ( *(p+1) )   /* not EOS */
                pd = p+1;
        }
        ++p;
    }

    if ( pd )
        strcpy(dst, pd);
    else
        strcpy(dst, path);

//    DBG("exec name [%s]", dst);
}


/* --------------------------------------------------------------------------
   Update G_now, G_ptm and G_dt
-------------------------------------------------------------------------- */
void lib_update_time_globals()
{
    G_now = time(NULL);
    G_ptm = gmtime(&G_now);
    sprintf(G_dt, "%d-%02d-%02d %02d:%02d:%02d", G_ptm->tm_year+1900, G_ptm->tm_mon+1, G_ptm->tm_mday, G_ptm->tm_hour, G_ptm->tm_min, G_ptm->tm_sec);
}


#ifdef HTTPS
/* --------------------------------------------------------------------------
   Log SSL error
-------------------------------------------------------------------------- */
static void log_ssl()
{
    char buf[256];
    u_long err;

    while ( (err=ERR_get_error()) != 0 )
    {
        ERR_error_string_n(err, buf, sizeof(buf));
        ERR(buf);
    }
}
#endif  /* HTTPS */


/* --------------------------------------------------------------------------
   Init SSL for a client
-------------------------------------------------------------------------- */
static bool init_ssl_client()
{
#ifdef HTTPS
    const SSL_METHOD *method;

    DBG("init_ssl (silgy_lib)");

    if ( !G_ssl_lib_initialized )
    {
        DBG("Initializing SSL_lib...");

        /* libssl init */
        SSL_library_init();
        SSL_load_error_strings();

        /* libcrypto init */
        OpenSSL_add_all_algorithms();
        ERR_load_crypto_strings();

        G_ssl_lib_initialized = TRUE;
    }

    method = SSLv23_client_method();    /* negotiate the highest protocol version supported by both the server and the client */

    M_ssl_ctx = SSL_CTX_new(method);    /* create new context from method */

    if ( M_ssl_ctx == NULL )
    {
        ERR("SSL_CTX_new failed");
        return FALSE;
    }

//    const long flags = SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
//    SSL_CTX_set_options(M_ssl_ctx, flags);

    /* temporarily ignore server cert errors */

    WAR("Ignoring remote server cert errors for REST calls");
//    SSL_CTX_set_verify(M_ssl_ctx, SSL_VERIFY_NONE, NULL);

#endif  /* HTTPS */
    return TRUE;
}


/* --------------------------------------------------------------------------
   Set socket as non-blocking
-------------------------------------------------------------------------- */
void lib_setnonblocking(int sock)
{
#ifdef _WIN32   /* Windows */

    u_long mode = 1;  // 1 to enable non-blocking socket
    ioctlsocket(sock, FIONBIO, &mode);

#else   /* Linux / UNIX */

    int opts;

    opts = fcntl(sock, F_GETFL);

    if ( opts < 0 )
    {
        ERR("fcntl(F_GETFL) failed");
        return;
    }

    opts = (opts | O_NONBLOCK);

    if ( fcntl(sock, F_SETFL, opts) < 0 )
    {
        ERR("fcntl(F_SETFL) failed");
        return;
    }
#endif
}


/* --------------------------------------------------------------------------
   REST call -- reset request headers
-------------------------------------------------------------------------- */
void lib_rest_headers_reset()
{
    M_rest_headers_cnt = 0;
}


/* --------------------------------------------------------------------------
   REST call -- set request header value
-------------------------------------------------------------------------- */
void lib_rest_header_set(const char *key, const char *value)
{
    int i;

    for ( i=0; i<M_rest_headers_cnt; ++i )
    {
        if ( M_rest_headers[i].key[0]==EOS )
        {
            strncpy(M_rest_headers[M_rest_headers_cnt].key, key, REST_HEADER_KEY_LEN);
            M_rest_headers[M_rest_headers_cnt].key[REST_HEADER_KEY_LEN] = EOS;
            strncpy(M_rest_headers[i].value, value, REST_HEADER_VAL_LEN);
            M_rest_headers[i].value[REST_HEADER_VAL_LEN] = EOS;
            return;
        }
        else if ( 0==strcmp(M_rest_headers[i].key, key) )
        {
            strncpy(M_rest_headers[i].value, value, REST_HEADER_VAL_LEN);
            M_rest_headers[i].value[REST_HEADER_VAL_LEN] = EOS;
            return;
        }
    }

    if ( M_rest_headers_cnt >= REST_MAX_HEADERS ) return;

    strncpy(M_rest_headers[M_rest_headers_cnt].key, key, REST_HEADER_KEY_LEN);
    M_rest_headers[M_rest_headers_cnt].key[REST_HEADER_KEY_LEN] = EOS;
    strncpy(M_rest_headers[M_rest_headers_cnt].value, value, REST_HEADER_VAL_LEN);
    M_rest_headers[M_rest_headers_cnt].value[REST_HEADER_VAL_LEN] = EOS;
    ++M_rest_headers_cnt;
}


/* --------------------------------------------------------------------------
   REST call -- unset request header value
-------------------------------------------------------------------------- */
void lib_rest_header_unset(const char *key)
{
    int i;

    for ( i=0; i<M_rest_headers_cnt; ++i )
    {
        if ( 0==strcmp(M_rest_headers[i].key, key) )
        {
            M_rest_headers[i].key[0] = EOS;
            M_rest_headers[i].value[0] = EOS;
            return;
        }
    }
}


/* --------------------------------------------------------------------------
   REST call / close connection
-------------------------------------------------------------------------- */
static void close_conn(int sock)
{
#ifdef _WIN32   /* Windows */
    closesocket(sock);
#else
    close(sock);
#endif  /* _WIN32 */
}


/* --------------------------------------------------------------------------
   REST call / parse URL
-------------------------------------------------------------------------- */
static bool rest_parse_url(const char *url, char *host, char *port, char *uri, bool *secure)
{
    int len = strlen(url);

    if ( len < 1 )
    {
        ERR("url too short (1)");
        return FALSE;
    }

//    if ( url[len-1] == '/' ) endingslash = TRUE;

    if ( len > 6 && url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p' && url[4]==':' )
    {
        url += 7;
        len -= 7;
        if ( len < 1 )
        {
            ERR("url too short (2)");
            return FALSE;
        }
    }
    else if ( len > 7 && url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p' && url[4]=='s' && url[5]==':' )
    {
#ifdef HTTPS
        *secure = TRUE;

        url += 8;
        len -= 8;
        if ( len < 1 )
        {
            ERR("url too short (2)");
            return FALSE;
        }

        if ( !M_ssl_ctx && !init_ssl_client() )   /* first time */
        {
            ERR("init_ssl failed");
            return FALSE;
        }
#else
        ERR("HTTPS is not enabled");
        return FALSE;
#endif  /* HTTPS */
    }

#ifdef DUMP
    DBG("url [%s]", url);
#endif

    char *colon, *slash;

    colon = strchr((char*)url, ':');
    slash = strchr((char*)url, '/');

    if ( colon )    /* port specified */
    {
        strncpy(host, url, colon-url);
        host[colon-url] = EOS;

        if ( slash )
        {
            strncpy(port, colon+1, slash-colon-1);
            port[slash-colon-1] = EOS;
            strcpy(uri, slash+1);
        }
        else    /* only host name & port */
        {
            strcpy(port, colon+1);
            uri[0] = EOS;
        }
    }
    else    /* no port specified */
    {
        if ( slash )
        {
            strncpy(host, url, slash-url);
            host[slash-url] = EOS;
            strcpy(uri, slash+1);
        }
        else    /* only host name */
        {
            strcpy(host, url);
            uri[0] = EOS;
        }
#ifdef HTTPS
        if ( *secure )
            strcpy(port, "443");
        else
#endif  /* HTTPS */
        strcpy(port, "80");
    }
#ifdef DUMP
    DBG("secure=%d", *secure);
    DBG("host [%s]", host);
    DBG("port [%s]", port);
    DBG(" uri [%s]", uri);
#endif
    return TRUE;
}


/* --------------------------------------------------------------------------
   REST call / return true if header is already present
-------------------------------------------------------------------------- */
static bool rest_header_present(const char *key)
{
    int i;
    char uheader[MAX_LABEL_LEN+1];

    strcpy(uheader, upper(key));

    for ( i=0; i<M_rest_headers_cnt; ++i )
    {
        if ( 0==strcmp(upper(M_rest_headers[i].key), uheader) )
        {
            return TRUE;
        }
    }

    return FALSE;
}


/* --------------------------------------------------------------------------
   REST call / set proxy
-------------------------------------------------------------------------- */
void lib_rest_proxy(bool value)
{
    M_rest_proxy = value;
}


/* --------------------------------------------------------------------------
   REST call / render request
-------------------------------------------------------------------------- */
static int rest_render_req(char *buffer, const char *method, const char *host, const char *uri, const void *req, bool json, bool keep)
{
    char *p=buffer;     /* stpcpy is faster than strcat */

    /* header */

    p = stpcpy(p, method);

    if ( M_rest_proxy )
        p = stpcpy(p, " ");
    else
        p = stpcpy(p, " /");

    p = stpcpy(p, uri);
    p = stpcpy(p, " HTTP/1.1\r\n");
    p = stpcpy(p, "Host: ");
    p = stpcpy(p, host);
    p = stpcpy(p, "\r\n");

    if ( 0 != strcmp(method, "GET") && req )
    {
        if ( json )     /* JSON -> string conversion */
        {
            if ( !rest_header_present("Content-Type") )
                p = stpcpy(p, "Content-Type: application/json\r\n");

            strcpy(G_tmp, lib_json_to_string((JSON*)req));
        }
        else
        {
            if ( !rest_header_present("Content-Type") )
                p = stpcpy(p, "Content-Type: application/x-www-form-urlencoded\r\n");
        }
        char tmp[64];
        sprintf(tmp, "Content-Length: %ld\r\n", (long)strlen(json?G_tmp:(char*)req));
        p = stpcpy(p, tmp);
    }

    if ( json && !rest_header_present("Accept") )
        p = stpcpy(p, "Accept: application/json\r\n");

/*    if ( !rest_header_present("Accept-Encoding") )
        p = stpcpy(p, "Accept-Encoding: gzip, deflate, br\r\n");

    p = stpcpy(p, "Pragma: no-cache\r\n");
    p = stpcpy(p, "Cache-Control: no-cache\r\n");
    p = stpcpy(p, "Accept-Language: en-GB,en;q=0.9\r\n"); */

    int i;

    for ( i=0; i<M_rest_headers_cnt; ++i )
    {
        if ( M_rest_headers[i].key[0] )
        {
            p = stpcpy(p, M_rest_headers[i].key);
            p = stpcpy(p, ": ");
            p = stpcpy(p, M_rest_headers[i].value);
            p = stpcpy(p, "\r\n");
        }
    }

    if ( keep )
        p = stpcpy(p, "Connection: keep-alive\r\n");
    else
        p = stpcpy(p, "Connection: close\r\n");

#ifndef NO_IDENTITY
    if ( !rest_header_present("User-Agent") )
#ifdef SILGY_AS_BOT
        p = stpcpy(p, "User-Agent: Silgy Bot\r\n");
#else
        p = stpcpy(p, "User-Agent: Silgy\r\n");
#endif
#endif  /* NO_IDENTITY */

    /* end of headers */

    p = stpcpy(p, "\r\n");

    /* body */

    if ( 0 != strcmp(method, "GET") && req )
        p = stpcpy(p, json?G_tmp:(char*)req);

    *p = EOS;

    return p - buffer;
}


/* --------------------------------------------------------------------------
   REST call / connect
-------------------------------------------------------------------------- */
static bool rest_connect(const char *host, const char *port, struct timespec *start, int *timeout_remain, bool secure)
{
static struct {
    char host[256];
    char port[16];
    struct addrinfo addr;
    struct sockaddr ai_addr;
} addresses[REST_ADDRESSES_CACHE_SIZE];
static int addresses_cnt=0, addresses_last=0;
    int  i;
    bool addr_cached=FALSE;

    DBG("rest_connect [%s:%s]", host, port);

    struct addrinfo *result=NULL;

#ifndef REST_CALL_DONT_CACHE_ADDRINFO

    DBG("Trying cache...");

#ifdef DUMP
    DBG("addresses_cnt=%d", addresses_cnt);
#endif

    for ( i=0; i<addresses_cnt; ++i )
    {
        if ( 0==strcmp(addresses[i].host, host) && 0==strcmp(addresses[i].port, port) )
        {
            DBG("Host [%s:%s] found in cache (%d)", host, port, i);
            addr_cached = TRUE;
            result = &addresses[i].addr;
            break;
        }
    }

#endif  /* REST_CALL_DONT_CACHE_ADDRINFO */

    if ( !addr_cached )
    {
#ifndef REST_CALL_DONT_CACHE_ADDRINFO
        DBG("Host [%s:%s] not found in cache", host, port);
#endif
        DBG("getaddrinfo...");   /* TODO: change to asynchronous, i.e. getaddrinfo_a */

        struct addrinfo hints;
        int s;

        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if ( (s=getaddrinfo(host, port, &hints, &result)) != 0 )
        {
            ERR("getaddrinfo: %s", gai_strerror(s));
            return FALSE;
        }

#ifdef DUMP
        DBG("elapsed after getaddrinfo: %.3lf ms", lib_elapsed(start));
#endif
        *timeout_remain = G_RESTTimeout - lib_elapsed(start);
        if ( *timeout_remain < 1 ) *timeout_remain = 1;

        /* getaddrinfo() returns a list of address structures.
           Try each address until we successfully connect */
    }

    DBG("Trying to connect...");

    struct addrinfo *rp;

    for ( rp=result; rp!=NULL; rp=rp->ai_next )
    {
#ifdef DUMP
        DBG("Trying socket...");
#endif
        M_rest_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (M_rest_sock == -1) continue;
#ifdef DUMP
        DBG("socket succeeded");
        DBG("elapsed after socket: %.3lf ms", lib_elapsed(start));
#endif
        *timeout_remain = G_RESTTimeout - lib_elapsed(start);
        if ( *timeout_remain < 1 ) *timeout_remain = 1;

        /* Windows timeout option is a s**t -- go for non-blocking I/O */

        lib_setnonblocking(M_rest_sock);

        int timeout_tmp = G_RESTTimeout/5;

        /* plain socket connection --------------------------------------- */

        if ( connect(M_rest_sock, rp->ai_addr, rp->ai_addrlen) != -1 )
        {
            break;  /* immediate success */
        }
        else if ( lib_finish_with_timeout(M_rest_sock, CONNECT, NULL, 0, &timeout_tmp, NULL, 0) == 0 )
        {
            break;  /* success within timeout */
        }

        close_conn(M_rest_sock);   /* no cigar */
    }

    if ( rp == NULL )   /* no address succeeded */
    {
        ERR("Could not connect");
        if ( result && !addr_cached ) freeaddrinfo(result);
        return FALSE;
    }

    /* -------------------------------------------------------------------------- */

    *timeout_remain = G_RESTTimeout - lib_elapsed(start);
    if ( *timeout_remain < 1 ) *timeout_remain = 1;

    /* -------------------------------------------------------------------------- */

    if ( !addr_cached )   /* add to cache */
    {
#ifndef REST_CALL_DONT_CACHE_ADDRINFO
        strcpy(addresses[addresses_last].host, host);
        strcpy(addresses[addresses_last].port, port);
        memcpy(&addresses[addresses_last].addr, rp, sizeof(struct addrinfo));
        /* addrinfo contains pointers -- mind the shallow copy! */
        memcpy(&addresses[addresses_last].ai_addr, rp->ai_addr, sizeof(struct sockaddr));
        addresses[addresses_last].addr.ai_addr = &addresses[addresses_last].ai_addr;
        addresses[addresses_last].addr.ai_next = NULL;

        /* get the remote address */
        char remote_addr[INET_ADDRSTRLEN]="";
        struct sockaddr_in *remote_addr_struct = (struct sockaddr_in*)rp->ai_addr;
#ifdef _WIN32   /* Windows */
        strcpy(remote_addr, inet_ntoa(remote_addr_struct->sin_addr));
#else
        inet_ntop(AF_INET, &(remote_addr_struct->sin_addr), remote_addr, INET_ADDRSTRLEN);
#endif
        INF("Connected to %s", remote_addr);

        DBG("Host [%s:%s] added to cache (%d)", host, port, addresses_last);

        if ( addresses_cnt < REST_ADDRESSES_CACHE_SIZE-1 )   /* first round */
        {
            ++addresses_cnt;    /* cache usage */
            ++addresses_last;   /* next to use index */
        }
        else    /* cache full -- reuse it from start */
        {
            if ( addresses_last < REST_ADDRESSES_CACHE_SIZE-1 )
                ++addresses_last;
            else
                addresses_last = 0;
        }

#endif  /* REST_CALL_DONT_CACHE_ADDRINFO */

        freeaddrinfo(result);
    }

    DBG("Connected");

#ifdef DUMP
    DBG("elapsed after plain connect: %.3lf ms", lib_elapsed(start));
#endif

    /* -------------------------------------------------------------------------- */

#ifdef HTTPS
    if ( secure )
    {
        DBG("Trying SSL_new...");

        M_rest_ssl = SSL_new(M_ssl_ctx);

        if ( !M_rest_ssl )
        {
            ERR("SSL_new failed");
            close_conn(M_rest_sock);
            return FALSE;
        }

        DBG("Trying SSL_set_fd...");

        int ret = SSL_set_fd(M_rest_ssl, M_rest_sock);

        if ( ret <= 0 )
        {
            ERR("SSL_set_fd failed, ret = %d", ret);
            close_conn(M_rest_sock);
            SSL_free(M_rest_ssl);
            M_rest_ssl = NULL;
            return FALSE;
        }

        DBG("Trying SSL_connect...");

        ret = SSL_connect(M_rest_ssl);
#ifdef DUMP
        DBG("ret = %d", ret);    /* 1 = success */
#endif
        if ( ret == 1 )
        {
            DBG("SSL_connect immediate success");
        }
        else if ( lib_finish_with_timeout(M_rest_sock, CONNECT, NULL, ret, timeout_remain, M_rest_ssl, 0) > 0 )
        {
            DBG("SSL_connect successful");
        }
        else
        {
            ERR("SSL_connect failed");
            close_conn(M_rest_sock);
            SSL_free(M_rest_ssl);
            M_rest_ssl = NULL;
            return FALSE;
        }

#ifdef DUMP
        DBG("elapsed after SSL connect: %.3lf ms", lib_elapsed(start));
#endif
        X509 *server_cert;
        server_cert = SSL_get_peer_certificate(M_rest_ssl);
        if ( server_cert )
        {
            DBG("Got server certificate");
			X509_NAME *certname;
			certname = X509_NAME_new();
			certname = X509_get_subject_name(server_cert);
			DBG("server_cert [%s]", X509_NAME_oneline(certname, NULL, 0));
            X509_free(server_cert);
        }
        else
            WAR("Couldn't get server certificate");
    }
#endif  /* HTTPS */

    return TRUE;
}


/* --------------------------------------------------------------------------
   REST call / disconnect
-------------------------------------------------------------------------- */
static void rest_disconnect()
{
    DBG("rest_disconnect");

#ifdef HTTPS
    if ( M_rest_ssl )
    {
        SSL_free(M_rest_ssl);
        M_rest_ssl = NULL;
    }
#endif  /* HTTPS */

    close_conn(M_rest_sock);
}


/* --------------------------------------------------------------------------
   REST call / get response content length
-------------------------------------------------------------------------- */
static int rest_res_content_length(const char *buffer, int len)
{
    const char *p;

    if ( (p=strstr(buffer, "\nContent-Length: ")) == NULL ) return -1;

    if ( len < (p-buffer) + 18 ) return -1;

    char result_str[8];
    char i=0;

    p += 17;

    while ( isdigit(*p) && i<7 )
    {
        result_str[i++] = *p++;
    }

    result_str[i] = EOS;

#ifdef DUMP
    DBG("result_str [%s]", result_str);
#endif

    return atoi(result_str);
}


/* --------------------------------------------------------------------------
   REST call / convert chunked to normal content
   Return number of bytes written to res_content
-------------------------------------------------------------------------- */
static int chunked2content(char *res_content, const char *buffer, int src_len, int res_len)
{
    char *res=res_content;
    char chunk_size_str[8];
    int  chunk_size=src_len;
    const char *p=buffer;
    int  was_read=0, written=0;

    while ( chunk_size > 0 )    /* chunk by chunk */
    {
        /* get the chunk size */

        int i=0;

        while ( *p!='\r' && *p!='\n' && i<6 && i<src_len )
            chunk_size_str[i++] = *p++;

        chunk_size_str[i] = EOS;
#ifdef DUMP
        DBG("chunk_size_str [%s]", chunk_size_str);
#endif
        sscanf(chunk_size_str, "%x", &chunk_size);
        DBG("chunk_size = %d", chunk_size);

        was_read += i;

        /* --------------------------------------------------------------- */

        if ( chunk_size == 0 )    /* last one */
        {
            DBG("Last chunk");
            break;
        }
        else if ( chunk_size < 0 )
        {
            ERR("chunk_size < 0");
            break;
        }
        else if ( chunk_size > res_len-written )
        {
            ERR("chunk_size > res_len-written");
            break;
        }

        /* --------------------------------------------------------------- */
        /* skip "\r\n" */

        p += 2;
        was_read += 2;

        /* --------------------------------------------------------------- */

        if ( was_read >= src_len || chunk_size > src_len-was_read )
        {
            ERR("Unexpected end of buffer");
            break;
        }

        /* --------------------------------------------------------------- */
        /* copy chunk to destination */

        res = stpncpy(res, p, chunk_size);
        written += chunk_size;

        p += chunk_size;
        was_read += chunk_size;

        /* --------------------------------------------------------------- */

        while ( *p != '\n' && was_read<src_len-1 )
        {
//            DBG("Skip '%c'", *p);
            ++p;
        }

        ++p;    /* skip '\n' */
        ++was_read;
    }

    return written;
}


/* --------------------------------------------------------------------------
   REST call / parse response
-------------------------------------------------------------------------- */
bool lib_rest_res_parse(char *res_header, int bytes)
{
    /* HTTP/1.1 200 OK <== 15 chars */

    char status[4];

    if ( bytes > 14 && 0==strncmp(res_header, "HTTP/1.", 7) )
    {
        res_header[bytes] = EOS;
#ifdef DUMP
        DBG("");
        DBG("Got %d bytes of response [%s]", bytes, res_header);
#else
        DBG("Got %d bytes of response", bytes);
#endif  /* DUMP */

        /* Status */

        strncpy(status, res_header+9, 3);
        status[3] = EOS;
        G_rest_status = atoi(status);
        INF("REST response status: %s", status);

        /* Content-Type */

        const char *p;

        if ( (p=strstr(res_header, "\nContent-Type: ")) == NULL
                && (p=strstr(res_header, "\nContent-type: ")) == NULL
                && (p=strstr(res_header, "\ncontent-type: ")) == NULL )
        {
            G_rest_content_type[0] = EOS;
            return TRUE;
        }

        if ( bytes < (p-res_header) + 16 )
        {
            G_rest_content_type[0] = EOS;
            return TRUE;
        }

        char i=0;

        p += 15;

        while ( *p != '\r' && *p != '\n' && *p && i<255 )
        {
            G_rest_content_type[i++] = *p++;
        }

        G_rest_content_type[i] = EOS;
        DBG("REST content type [%s]", G_rest_content_type);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}


/* --------------------------------------------------------------------------
   REST call
-------------------------------------------------------------------------- */
bool lib_rest_req(const void *req, void *res, const char *method, const char *url, bool json, bool keep)
{
    char    host[256];
    char    port[8];
    bool    secure=FALSE;
static char prev_host[256];
static char prev_port[8];
static bool prev_secure=FALSE;
    char    uri[MAX_URI_LEN+1];
static bool connected=FALSE;
static time_t connected_time=0;
    char    res_header[REST_RES_HEADER_LEN+1];
static char buffer[JSON_BUFSIZE];
    long    bytes;
    char    *body;
    int     content_read=0, buffer_read=0;
    int     len, i, j;
    int     timeout_remain = G_RESTTimeout;
#ifdef HTTPS
    int     ssl_err;
#endif  /* HTTPS */

    DBG("lib_rest_req [%s] [%s]", method, url);

    /* -------------------------------------------------------------------------- */

    if ( !rest_parse_url(url, host, port, uri, &secure) ) return FALSE;

    if ( M_rest_proxy )
        strcpy(uri, url);

    len = rest_render_req(buffer, method, host, uri, req, json, keep);

#ifdef DUMP
    DBG("------------------------------------------------------------");
    DBG("lib_rest_req buffer [%s]", buffer);
    DBG("------------------------------------------------------------");
#endif  /* DUMP */

    struct timespec start;
#ifdef _WIN32
    clock_gettime_win(&start);
#else
    clock_gettime(MONOTONIC_CLOCK_NAME, &start);
#endif

    /* -------------------------------------------------------------------------- */

    if ( connected
            && (secure!=prev_secure || 0 != strcmp(host, prev_host) || 0 != strcmp(port, prev_port) || G_now-connected_time > CONN_TIMEOUT) )
    {
        /* reconnect anyway */
        DBG("different host, port or old connection, reconnecting");
        rest_disconnect();
        connected = FALSE;
    }

    bool was_connected = connected;

    /* connect if necessary ----------------------------------------------------- */

    if ( !connected && !rest_connect(host, port, &start, &timeout_remain, secure) ) return FALSE;

    /* -------------------------------------------------------------------------- */

    DBG("Sending request...");

    bool after_reconnect=0;

    while ( timeout_remain > 1 )
    {
#ifdef HTTPS
        if ( secure )
        {
/*            char first_char[2];
            first_char[0] = buffer[0];
            first_char[1] = EOS;

            bytes = SSL_write(M_rest_ssl, first_char, 1);

            if ( bytes > 0 )
                bytes = SSL_write(M_rest_ssl, buffer+1, len-1) + bytes; */

            bytes = SSL_write(M_rest_ssl, buffer, len);
        }
        else
#endif  /* HTTPS */
            bytes = send(M_rest_sock, buffer, len, 0);    /* try in one go */

        if ( !secure && bytes <= 0 )
        {
            if ( !was_connected || after_reconnect )
            {
                ERR("Send (after fresh connect) failed");
                rest_disconnect();
                connected = FALSE;
                return FALSE;
            }

            DBG("Disconnected? Trying to reconnect...");
            rest_disconnect();
            if ( !rest_connect(host, port, &start, &timeout_remain, secure) ) return FALSE;
            after_reconnect = 1;
        }
        else if ( secure && bytes == -1 )
        {
            bytes = lib_finish_with_timeout(M_rest_sock, WRITE, buffer, len, &timeout_remain, secure?M_rest_ssl:NULL, 0);

            if ( bytes == -1 )
            {
                if ( !was_connected || after_reconnect )
                {
                    ERR("Send (after fresh connect) failed");
                    rest_disconnect();
                    connected = FALSE;
                    return FALSE;
                }

                DBG("Disconnected? Trying to reconnect...");
                rest_disconnect();
                if ( !rest_connect(host, port, &start, &timeout_remain, secure) ) return FALSE;
                after_reconnect = 1;
            }
        }
        else    /* bytes > 0 ==> OK */
        {
            break;
        }
    }

#ifdef DUMP
    DBG("Sent %ld bytes", bytes);
#endif

    if ( bytes < 15 )
    {
        ERR("send failed, errno = %d (%s)", errno, strerror(errno));
        rest_disconnect();
        connected = FALSE;
        return FALSE;
    }

#ifdef DUMP
    DBG("elapsed after request: %.3lf ms", lib_elapsed(&start));
#endif

    /* -------------------------------------------------------------------------- */

    DBG("Reading response...");

#ifdef HTTPS
    if ( secure )
        bytes = SSL_read(M_rest_ssl, res_header, REST_RES_HEADER_LEN);
    else
#endif  /* HTTPS */
        bytes = recv(M_rest_sock, res_header, REST_RES_HEADER_LEN, 0);

    if ( bytes == -1 )
    {
        bytes = lib_finish_with_timeout(M_rest_sock, READ, res_header, REST_RES_HEADER_LEN, &timeout_remain, secure?M_rest_ssl:NULL, 0);

        if ( bytes <= 0 )
        {
            ERR("recv failed, errno = %d (%s)", errno, strerror(errno));
            rest_disconnect();
            connected = FALSE;
            return FALSE;
        }
    }

    DBG("Read %ld bytes", bytes);

#ifdef DUMP
    DBG("elapsed after first response read: %.3lf ms", lib_elapsed(&start));
#endif

    /* -------------------------------------------------------------------------- */
    /* parse the response                                                         */
    /* we assume that at least response header arrived at once                    */

    if ( !lib_rest_res_parse(res_header, bytes) )
    {
        ERR("No or incomplete response");
#ifdef DUMP
        if ( bytes >= 0 )
        {
            res_header[bytes] = EOS;
            DBG("Got %d bytes of response [%s]", bytes, res_header);
        }
#endif
        G_rest_status = 500;
        rest_disconnect();
        connected = FALSE;
        return FALSE;
    }

    /* ------------------------------------------------------------------- */
    /* at this point we've got something that seems to be a HTTP header,
       possibly with content */

    /* ------------------------------------------------------------------- */
    /* find the expected Content-Length                                    */

    int content_length = rest_res_content_length(res_header, bytes);

    if ( content_length > JSON_BUFSIZE-1 )
    {
        ERR("Response content is too big (%d)", content_length);
        rest_disconnect();
        connected = FALSE;
        return FALSE;
    }

    /* ------------------------------------------------------------------- */
    /*
       There are 4 options now:

       1. Normal content with explicit Content-Length (content_length > 0)
       2. No content gracefully (content_length = 0)
       3. Chunked content (content_length = -1 and Transfer-Encoding says 'chunked')
       4. Error -- that is, no Content-Length header and no Transfer-Encoding

    */

#define TRANSFER_MODE_NORMAL     '1'
#define TRANSFER_MODE_NO_CONTENT '2'
#define TRANSFER_MODE_CHUNKED    '3'
#define TRANSFER_MODE_ERROR      '4'

static char res_content[JSON_BUFSIZE];
    char mode;

    if ( content_length > 0 )     /* Content-Length present in response */
    {
        DBG("TRANSFER_MODE_NORMAL");
        mode = TRANSFER_MODE_NORMAL;
    }
    else if ( content_length == 0 )
    {
        DBG("TRANSFER_MODE_NO_CONTENT");
        mode = TRANSFER_MODE_NO_CONTENT;
    }
    else    /* content_length == -1 */
    {
        if ( strstr(res_header, "\nTransfer-Encoding: chunked") != NULL )
        {
            DBG("TRANSFER_MODE_CHUNKED");
            mode = TRANSFER_MODE_CHUNKED;
        }
        else
        {
            WAR("TRANSFER_MODE_ERROR");
            mode = TRANSFER_MODE_ERROR;
        }
    }

    /* ------------------------------------------------------------------- */
    /* some content may have already been read                             */

    body = strstr(res_header, "\r\n\r\n");

    if ( body )
    {
        body += 4;

        int was_read = bytes - (body-res_header);

        if ( was_read > 0 )
        {
            if ( mode == TRANSFER_MODE_NORMAL )   /* not chunked goes directly to res_content */
            {
                content_read = was_read;
                strncpy(res_content, body, content_read);
            }
            else if ( mode == TRANSFER_MODE_CHUNKED )   /* chunked goes to buffer before res_content */
            {
                buffer_read = was_read;
                strncpy(buffer, body, buffer_read);
            }
        }
    }

    /* ------------------------------------------------------------------- */
    /* read content                                                        */

    if ( mode == TRANSFER_MODE_NORMAL )
    {
        while ( content_read < content_length && timeout_remain > 1 )   /* read whatever we can within timeout */
        {
#ifdef DUMP
            DBG("trying again (content-length)");
#endif

#ifdef HTTPS
            if ( secure )
                bytes = SSL_read(M_rest_ssl, res_content+content_read, JSON_BUFSIZE-content_read-1);
            else
#endif  /* HTTPS */
                bytes = recv(M_rest_sock, res_content+content_read, JSON_BUFSIZE-content_read-1, 0);

            if ( bytes == -1 )
                bytes = lib_finish_with_timeout(M_rest_sock, READ, res_content+content_read, JSON_BUFSIZE-content_read-1, &timeout_remain, secure?M_rest_ssl:NULL, 0);

            if ( bytes > 0 )
                content_read += bytes;
        }

        if ( bytes < 1 )
        {
            DBG("timeouted?");
            rest_disconnect();
            connected = FALSE;
            return FALSE;
        }
    }
    else if ( mode == TRANSFER_MODE_CHUNKED )
    {
        /* for single-threaded process, I can't see better option than to read everything
           into a buffer and then parse and copy chunks into final res_content */

        /* there's no guarantee that every read = one chunk, so just read whatever comes in, until it does */

        while ( bytes > 0 && timeout_remain > 1 )   /* read whatever we can within timeout */
        {
#ifdef DUMP
            DBG("Has the last chunk been read?");
            DBG("buffer_read = %d", buffer_read);
            if ( buffer_read > 5 )
            {
                int ii;
                for ( ii=buffer_read-6; ii<buffer_read; ++ii )
                {
                    if ( buffer[ii] == '\r' )
                        DBG("buffer[%d] '\\r'", ii);
                    else if ( buffer[ii] == '\n' )
                        DBG("buffer[%d] '\\n'", ii);
                    else
                        DBG("buffer[%d] '%c'", ii, buffer[ii]);
                }
            }
#endif
            if ( buffer_read>5 && buffer[buffer_read-6]=='\n' && buffer[buffer_read-5]=='0' && buffer[buffer_read-4]=='\r' && buffer[buffer_read-3]=='\n' && buffer[buffer_read-2]=='\r' && buffer[buffer_read-1]=='\n' )
            {
                DBG("Last chunk detected (with \\r\\n\\r\\n)");
                break;
            }
            else if ( buffer_read>3 && buffer[buffer_read-4]=='\n' && buffer[buffer_read-3]=='0' && buffer[buffer_read-2]=='\r' && buffer[buffer_read-1]=='\n' )
            {
                DBG("Last chunk detected (with \\r\\n)");
                break;
            }
/*            else if ( buffer_read>1 && buffer[buffer_read-2]=='\n' && buffer[buffer_read-1]=='0' )
            {
                DBG("Last chunk detected (without \\r\\n)");
                break;
            } */

#ifdef DUMP
            DBG("trying again (chunked)");
#endif

#ifdef HTTPS
            if ( secure )
                bytes = SSL_read(M_rest_ssl, buffer+buffer_read, JSON_BUFSIZE-buffer_read-1);
            else
#endif  /* HTTPS */
                bytes = recv(M_rest_sock, buffer+buffer_read, JSON_BUFSIZE-buffer_read-1, 0);

            if ( bytes == -1 )
                bytes = lib_finish_with_timeout(M_rest_sock, READ, buffer+buffer_read, JSON_BUFSIZE-buffer_read-1, &timeout_remain, secure?M_rest_ssl:NULL, 0);

            if ( bytes > 0 )
                buffer_read += bytes;
        }

        if ( buffer_read < 5 )
        {
            ERR("buffer_read is only %d, this can't be valid chunked content", buffer_read);
            rest_disconnect();
            connected = FALSE;
            return FALSE;
        }

//        buffer[buffer_read] = EOS;

        content_read = chunked2content(res_content, buffer, buffer_read, JSON_BUFSIZE);
    }

    /* ------------------------------------------------------------------- */

    res_content[content_read] = EOS;

    DBG("Read %d bytes of content", content_read);

#ifdef DUMP
    log_long(res_content, content_read, "Content");
#endif

    /* ------------------------------------------------------------------- */

    if ( !keep || strstr(res_header, "\nConnection: close") != NULL )
    {
        DBG("Closing connection");
        rest_disconnect();
        connected = FALSE;
    }
    else    /* keep the connection open */
    {
#ifdef HTTPS
        prev_secure = secure;
#endif
        strcpy(prev_host, host);
        strcpy(prev_port, port);
        connected = TRUE;
        connected_time = G_now;
    }

#ifdef DUMP
    DBG("elapsed after second response read: %.3lf ms", lib_elapsed(&start));
#endif

    /* -------------------------------------------------------------------------- */
    /* we expect JSON response in body                                            */

    if ( len && res )
    {
        if ( json )
            lib_json_from_string((JSON*)res, res_content, content_read, 0);
        else
            strcpy((char*)res, res_content);
    }

    /* -------------------------------------------------------------------------- */
    /* stats                                                                      */

    ++G_rest_req;

    double elapsed = lib_elapsed(&start);

    DBG("REST call finished in %.3lf ms", elapsed);

    G_rest_elapsed += elapsed;

    G_rest_average = G_rest_elapsed / G_rest_req;

    return TRUE;
}


/* --------------------------------------------------------------------------
   Finish socket operation with timeout
-------------------------------------------------------------------------- */
int lib_finish_with_timeout(int sock, char readwrite, char *buffer, int len, int *msec, void *ssl, int level)
{
    int             sockerr;
    struct timeval  timeout;
    fd_set          readfds;
    fd_set          writefds;
    int             socks=0;

#ifdef DUMP
    if ( readwrite==READ )
        DBG("lib_finish_with_timeout READ");
    else if ( readwrite==WRITE )
        DBG("lib_finish_with_timeout WRITE");
    else
        DBG("lib_finish_with_timeout CONNECT");
#endif

    if ( level > 10 )
    {
        ERR("lib_finish_with_timeout -- too many levels");
        return -1;
    }

    /* get the error code ------------------------------------------------ */
    /* note: during SSL operations it will be 0                            */

    if ( !ssl )
    {
#ifdef _WIN32   /* Windows */

        sockerr = WSAGetLastError();

        if ( sockerr != WSAEWOULDBLOCK )
        {
            wchar_t *s = NULL;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, sockerr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&s, 0, NULL);
            ERR("%d (%S)", sockerr, s);
            LocalFree(s);
            return -1;
        }

#else   /* Linux */

        sockerr = errno;

        if ( sockerr != EWOULDBLOCK && sockerr != EINPROGRESS )
        {
            ERR("sockerr = %d (%s)", sockerr, strerror(sockerr));
            return -1;
        }

#endif  /* _WIN32 */
    }

    /* set up timeout for select ----------------------------------------- */

    if ( *msec < 1000 )
    {
        timeout.tv_sec = 0;
        timeout.tv_usec = *msec*1000;
    }
    else    /* 1000 ms or more */
    {
        timeout.tv_sec = *msec/1000;
        timeout.tv_usec = (*msec-((int)(*msec/1000)*1000))*1000;
    }

    /* update remaining timeout ------------------------------------------ */

    struct timespec start;
#ifdef _WIN32
    clock_gettime_win(&start);
#else
    clock_gettime(MONOTONIC_CLOCK_NAME, &start);
#endif

    /* set up fd-s and wait ---------------------------------------------- */

#ifdef DUMP
    DBG("Waiting on select()...");
#endif

    if ( readwrite == READ )
    {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        socks = select(sock+1, &readfds, NULL, NULL, &timeout);
    }
    else    /* WRITE or CONNECT */
    {
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        socks = select(sock+1, NULL, &writefds, NULL, &timeout);
    }

    *msec -= lib_elapsed(&start);
    if ( *msec < 1 ) *msec = 1;
#ifdef DUMP
    DBG("msec reduced to %d ms", *msec);
#endif

    /* process select result --------------------------------------------- */

    if ( socks < 0 )
    {
        ERR("select failed, errno = %d (%s)", errno, strerror(errno));
        return -1;
    }
    else if ( socks == 0 )
    {
        WAR("lib_finish_with_timeout timeouted (was waiting for %.2f ms)", lib_elapsed(&start));
        return -1;
    }
    else    /* socket is ready for I/O */
    {
#ifdef DUMP
        DBG("lib_finish_with_timeout socks > 0");
#endif
#ifdef HTTPS
        int bytes, ssl_err;
        char ec[128]="";
#endif

        if ( readwrite == READ )
        {
#ifdef HTTPS
            if ( ssl )
            {
                if ( buffer )
                    bytes = SSL_read((SSL*)ssl, buffer, len);
                else    /* connect */
                    bytes = SSL_connect((SSL*)ssl);

                if ( bytes > 0 )
                {
                    return bytes;
                }
                else
                {
                    ssl_err = SSL_get_error((SSL*)ssl, bytes);
#ifdef DUMP
                    if ( ssl_err == SSL_ERROR_SYSCALL )
                        sprintf(ec, ", errno = %d (%s)", sockerr, strerror(sockerr));

                    DBG("bytes = %d, ssl_err = %d%s", bytes, ssl_err, ec);
#endif  /* DUMP */
                    if ( ssl_err==SSL_ERROR_WANT_READ )
                        return lib_finish_with_timeout(sock, READ, buffer, len, msec, ssl, level+1);
                    else if ( ssl_err==SSL_ERROR_WANT_WRITE )
                        return lib_finish_with_timeout(sock, WRITE, buffer, len, msec, ssl, level+1);
                    else
                    {
                        DBG("SSL_read error %d", ssl_err);
                        return -1;
                    }
                }
            }
            else
#endif  /* HTTPS */
                return recv(sock, buffer, len, 0);
        }
        else if ( readwrite == WRITE )
        {
#ifdef HTTPS
            if ( ssl )
            {
                if ( buffer )
                    bytes = SSL_write((SSL*)ssl, buffer, len);
                else    /* connect */
                    bytes = SSL_connect((SSL*)ssl);

                if ( bytes > 0 )
                {
                    return bytes;
                }
                else
                {
                    ssl_err = SSL_get_error((SSL*)ssl, bytes);
#ifdef DUMP
                    if ( ssl_err == SSL_ERROR_SYSCALL )
                        sprintf(ec, ", errno = %d (%s)", sockerr, strerror(sockerr));

                    DBG("bytes = %d, ssl_err = %d%s", bytes, ssl_err, ec);
#endif  /* DUMP */
                    if ( ssl_err==SSL_ERROR_WANT_WRITE )
                        return lib_finish_with_timeout(sock, WRITE, buffer, len, msec, ssl, level+1);
                    else if ( ssl_err==SSL_ERROR_WANT_READ )
                        return lib_finish_with_timeout(sock, READ, buffer, len, msec, ssl, level+1);
                    else
                    {
                        DBG("SSL_write error %d", ssl_err);
                        return -1;
                    }
                }
            }
            else
#endif  /* HTTPS */
                return send(sock, buffer, len, 0);
        }
        else   /* CONNECT */
        {
#ifdef HTTPS
            if ( ssl )
            {
                ssl_err = SSL_get_error((SSL*)ssl, len);
#ifdef DUMP
                if ( ssl_err == SSL_ERROR_SYSCALL )
                    sprintf(ec, ", errno = %d (%s)", sockerr, strerror(sockerr));

                DBG("error = %d, ssl_err = %d%s", len, ssl_err, ec);
#endif  /* DUMP */
                if ( ssl_err==SSL_ERROR_WANT_WRITE )
                    return lib_finish_with_timeout(sock, WRITE, NULL, 0, msec, ssl, level+1);
                else if ( ssl_err==SSL_ERROR_WANT_READ )
                    return lib_finish_with_timeout(sock, READ, NULL, 0, msec, ssl, level+1);
                else
                {
                    DBG("SSL_connect error %d", ssl_err);
                    return -1;
                }
            }
            else
#endif  /* HTTPS */
            return 0;
        }
    }
}


/* --------------------------------------------------------------------------
   Set G_appdir
-------------------------------------------------------------------------- */
void lib_get_app_dir()
{
    char *appdir=NULL;

    if ( NULL != (appdir=getenv("SILGYDIR")) )
    {
        strcpy(G_appdir, appdir);
        int len = strlen(G_appdir);
        if ( G_appdir[len-1] == '/' ) G_appdir[len-1] = EOS;
    }
    else
    {
        G_appdir[0] = EOS;   /* not defined */
    }
}


/* --------------------------------------------------------------------------
   Calculate elapsed time
-------------------------------------------------------------------------- */
double lib_elapsed(struct timespec *start)
{
struct timespec end;
    double      elapsed;
#ifdef _WIN32
    clock_gettime_win(&end);
#else
    clock_gettime(MONOTONIC_CLOCK_NAME, &end);
#endif
    elapsed = (end.tv_sec - start->tv_sec) * 1000.0;
    elapsed += (end.tv_nsec - start->tv_nsec) / 1000000.0;
    return elapsed;
}


/* --------------------------------------------------------------------------
   Log the memory footprint
-------------------------------------------------------------------------- */
void lib_log_memory()
{
    long        mem_used;
    char        mem_used_kb[32];
    char        mem_used_mb[32];
    char        mem_used_gb[32];

    mem_used = lib_get_memory();

    amt(mem_used_kb, mem_used);
    amtd(mem_used_mb, (double)mem_used/1024);
    amtd(mem_used_gb, (double)mem_used/1024/1024);

    ALWAYS_LINE;
    ALWAYS("Memory: %s kB (%s MB / %s GB)", mem_used_kb, mem_used_mb, mem_used_gb);
    ALWAYS_LINE;
}


/* --------------------------------------------------------------------------
   For lib_memory
-------------------------------------------------------------------------- */
static int mem_parse_line(const char* line)
{
    long    ret=0;
    int     i=0;
    char    strret[64];
    const char* p=line;

    while (!isdigit(*p)) ++p;       /* skip non-digits */

    while (isdigit(*p)) strret[i++] = *p++;

    strret[i] = EOS;

/*  DBG("mem_parse_line: line [%s]", line);
    DBG("mem_parse_line: strret [%s]", strret);*/

    ret = atol(strret);

    return ret;
}


/* --------------------------------------------------------------------------
   Return currently used memory (high water mark) by current process in kB
-------------------------------------------------------------------------- */
long lib_get_memory()
{
    long result=0;

#ifdef __linux__

    char line[128];
    FILE* file = fopen("/proc/self/status", "r");

    if ( !file )
    {
        ERR("fopen(\"/proc/self/status\" failed, errno = %d (%s)", errno, strerror(errno));
        return result;
    }

    while ( fgets(line, 128, file) != NULL )
    {
        if ( strncmp(line, "VmHWM:", 6) == 0 )
        {
            result = mem_parse_line(line);
            break;
        }
    }

    fclose(file);

#else   /* not Linux */

#ifdef _WIN32   /* Windows */

    PROCESS_MEMORY_COUNTERS_EX pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    result = pmc.PrivateUsage / 1024;

#else   /* UNIX */

struct rusage usage;

    getrusage(RUSAGE_SELF, &usage);
    result = usage.ru_maxrss;

#endif  /* _WIN32 */

#endif  /* __linux__ */

    return result;
}


/* --------------------------------------------------------------------------
   Filter everything but basic letters and digits off
---------------------------------------------------------------------------*/
char *silgy_filter_strict(const char *src)
{
static char dst[4096];
    int     i=0, j=0;

    while ( src[i] && j<4095 )
    {
        if ( (src[i] >= 65 && src[i] <= 90)
                || (src[i] >= 97 && src[i] <= 122)
                || isdigit(src[i]) )
            dst[j++] = src[i];
        else if ( src[i] == ' ' )
            dst[j++] = '_';

        ++i;
    }

    dst[j] = EOS;

    return dst;
}


/* --------------------------------------------------------------------------
   Add spaces to make string to have len length
-------------------------------------------------------------------------- */
char *lib_add_spaces(const char *src, int len)
{
static char ret[4096];
    int     src_len;
    int     spaces;
    int     i;

    src_len = strlen(src);

    spaces = len - src_len;

    if ( spaces < 0 ) spaces = 0;

    strcpy(ret, src);

    for ( i=src_len; i<len; ++i )
        ret[i] = ' ';

    ret[i] = EOS;

    return ret;
}


/* --------------------------------------------------------------------------
   Add leading spaces to make string to have len length
-------------------------------------------------------------------------- */
char *lib_add_lspaces(const char *src, int len)
{
static char ret[4096];
    int     src_len;
    int     spaces;
    int     i;

    src_len = strlen(src);

    spaces = len - src_len;

    if ( spaces < 0 ) spaces = 0;

    for ( i=0; i<spaces; ++i )
        ret[i] = ' ';

    strcpy(ret+spaces, src);

    return ret;
}


/* --------------------------------------------------------------------------
   Determine resource type by its extension
-------------------------------------------------------------------------- */
char get_res_type(const char *fname)
{
    char    *ext=NULL;
    char    uext[8]="";

#ifdef DUMP
//  DBG("name: [%s]", fname);
#endif

    if ( (ext=(char*)strrchr(fname, '.')) == NULL )     /* no dot */
        return RES_TEXT;

    if ( ext-fname == strlen(fname)-1 )             /* dot is the last char */
        return RES_TEXT;

    ++ext;

    if ( strlen(ext) > 4 )                          /* extension too long */
        return RES_TEXT;

#ifdef DUMP
//  DBG("ext: [%s]", ext);
#endif

    strcpy(uext, upper(ext));

    if ( 0==strcmp(uext, "HTML") || 0==strcmp(uext, "HTM") )
        return RES_HTML;
    else if ( 0==strcmp(uext, "CSS") )
        return RES_CSS;
    else if ( 0==strcmp(uext, "JS") )
        return RES_JS;
    else if ( 0==strcmp(uext, "PDF") )
        return RES_PDF;
    else if ( 0==strcmp(uext, "GIF") )
        return RES_GIF;
    else if ( 0==strcmp(uext, "JPG") )
        return RES_JPG;
    else if ( 0==strcmp(uext, "ICO") )
        return RES_ICO;
    else if ( 0==strcmp(uext, "PNG") )
        return RES_PNG;
    else if ( 0==strcmp(uext, "BMP") )
        return RES_BMP;
    else if ( 0==strcmp(uext, "SVG") )
        return RES_SVG;
    else if ( 0==strcmp(uext, "MP3") )
        return RES_AMPEG;
    else if ( 0==strcmp(uext, "EXE") )
        return RES_EXE;
    else if ( 0==strcmp(uext, "ZIP") )
        return RES_ZIP;

    return RES_TEXT;
}


/* --------------------------------------------------------------------------
  convert URI (YYYY-MM-DD) date to tm struct
-------------------------------------------------------------------------- */
void date_str2rec(const char *str, date_t *rec)
{
    int     len;
    int     i;
    int     j=0;
    char    part='Y';
    char    strtmp[8];

    len = strlen(str);

    /* empty or invalid date => return today */

    if ( len != 10 || str[4] != '-' || str[7] != '-' )
    {
        DBG("date_str2rec: empty or invalid date in URI, returning today");
        rec->year = G_ptm->tm_year+1900;
        rec->month = G_ptm->tm_mon+1;
        rec->day = G_ptm->tm_mday;
        return;
    }

    for (i=0; i<len; ++i)
    {
        if ( str[i] != '-' )
            strtmp[j++] = str[i];
        else    /* end of part */
        {
            strtmp[j] = EOS;
            if ( part == 'Y' )  /* year */
            {
                rec->year = atoi(strtmp);
                part = 'M';
            }
            else if ( part == 'M' ) /* month */
            {
                rec->month = atoi(strtmp);
                part = 'D';
            }
            j = 0;
        }
    }

    /* day */

    strtmp[j] = EOS;
    rec->day = atoi(strtmp);
}


/* --------------------------------------------------------------------------
   Convert date_t date to YYYY-MM-DD string
-------------------------------------------------------------------------- */
void date_rec2str(char *str, date_t *rec)
{
    sprintf(str, "%d-%02d-%02d", rec->year, rec->month, rec->day);
}


/* --------------------------------------------------------------------------
   Convert HTTP time to epoch
   Tue, 18 Oct 2016 13:13:03 GMT
   Thu, 24 Nov 2016 21:19:40 GMT
-------------------------------------------------------------------------- */
time_t time_http2epoch(const char *str)
{
    time_t  epoch;
    char    tmp[8];
struct tm   tm;
//  char    *temp;  // temporarily

    // temporarily
//  DBG("time_http2epoch in:  [%s]", str);

    if ( strlen(str) != 29 )
        return 0;

    /* day */

    strncpy(tmp, str+5, 2);
    tmp[2] = EOS;
    tm.tm_mday = atoi(tmp);

    /* month */

    strncpy(tmp, str+8, 3);
    tmp[3] = EOS;
    if ( 0==strcmp(tmp, "Feb") )
        tm.tm_mon = 1;
    else if ( 0==strcmp(tmp, "Mar") )
        tm.tm_mon = 2;
    else if ( 0==strcmp(tmp, "Apr") )
        tm.tm_mon = 3;
    else if ( 0==strcmp(tmp, "May") )
        tm.tm_mon = 4;
    else if ( 0==strcmp(tmp, "Jun") )
        tm.tm_mon = 5;
    else if ( 0==strcmp(tmp, "Jul") )
        tm.tm_mon = 6;
    else if ( 0==strcmp(tmp, "Aug") )
        tm.tm_mon = 7;
    else if ( 0==strcmp(tmp, "Sep") )
        tm.tm_mon = 8;
    else if ( 0==strcmp(tmp, "Oct") )
        tm.tm_mon = 9;
    else if ( 0==strcmp(tmp, "Nov") )
        tm.tm_mon = 10;
    else if ( 0==strcmp(tmp, "Dec") )
        tm.tm_mon = 11;
    else    /* January */
        tm.tm_mon = 0;

    /* year */

    strncpy(tmp, str+12, 4);
    tmp[4] = EOS;
    tm.tm_year = atoi(tmp) - 1900;

    /* hour */

    strncpy(tmp, str+17, 2);
    tmp[2] = EOS;
    tm.tm_hour = atoi(tmp);

    /* minute */

    strncpy(tmp, str+20, 2);
    tmp[2] = EOS;
    tm.tm_min = atoi(tmp);

    /* second */

    strncpy(tmp, str+23, 2);
    tmp[2] = EOS;
    tm.tm_sec = atoi(tmp);

//  DBG("%d-%02d-%02d %02d:%02d:%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

#ifdef __linux__
    epoch = timegm(&tm);
#else
    epoch = mktime(&tm);
#endif

    // temporarily
//  char *temp = time_epoch2http(epoch);
//  DBG("time_http2epoch out: [%s]", temp);

    return epoch;
}


/* --------------------------------------------------------------------------
   Convert db time to epoch
   2016-12-25 12:15:00
-------------------------------------------------------------------------- */
time_t time_db2epoch(const char *str)
{
    time_t  epoch;
    char    tmp[8];
struct tm   tm;

    if ( strlen(str) != 19 )
        return 0;

    /* year */

    strncpy(tmp, str, 4);
    tmp[4] = EOS;
    tm.tm_year = atoi(tmp) - 1900;

    /* month */

    strncpy(tmp, str+5, 2);
    tmp[2] = EOS;
    tm.tm_mon = atoi(tmp) - 1;

    /* day */

    strncpy(tmp, str+8, 2);
    tmp[2] = EOS;
    tm.tm_mday = atoi(tmp);

    /* hour */

    strncpy(tmp, str+11, 2);
    tmp[2] = EOS;
    tm.tm_hour = atoi(tmp);

    /* minute */

    strncpy(tmp, str+14, 2);
    tmp[2] = EOS;
    tm.tm_min = atoi(tmp);

    /* second */

    strncpy(tmp, str+17, 2);
    tmp[2] = EOS;
    tm.tm_sec = atoi(tmp);

#ifdef __linux__
    epoch = timegm(&tm);
#else
    epoch = mktime(&tm);
#endif

    return epoch;
}


/* --------------------------------------------------------------------------
   Convert epoch to HTTP time
-------------------------------------------------------------------------- */
char *time_epoch2http(time_t epoch)
{
static char str[32];

    G_ptm = gmtime(&epoch);
#ifdef _WIN32   /* Windows */
    strftime(str, 32, "%a, %d %b %Y %H:%M:%S GMT", G_ptm);
#else
    strftime(str, 32, "%a, %d %b %Y %T GMT", G_ptm);
#endif  /* _WIN32 */

    G_ptm = gmtime(&G_now);  /* make sure G_ptm is up to date */

//  DBG("time_epoch2http: [%s]", str);

    return str;
}


/* --------------------------------------------------------------------------
   Set decimal & thousand separator
---------------------------------------------------------------------------*/
void lib_set_datetime_formats(const char *lang)
{
    char ulang[8];

    DBG("lib_set_datetime_formats, lang [%s]", lang);

    strcpy(ulang, upper(lang));

    // date format

    if ( 0==strcmp(ulang, "EN-US") )
        M_df = 1;
    else if ( 0==strcmp(ulang, "EN-GB") || 0==strcmp(ulang, "EN-AU") || 0==strcmp(ulang, "FR-FR") || 0==strcmp(ulang, "EN-IE") || 0==strcmp(ulang, "ES-ES") || 0==strcmp(ulang, "IT-IT") || 0==strcmp(ulang, "PT-PT") || 0==strcmp(ulang, "PT-BR") || 0==strcmp(ulang, "ES-AR") )
        M_df = 2;
    else if ( 0==strcmp(ulang, "PL-PL") || 0==strcmp(ulang, "RU-RU") || 0==strcmp(ulang, "DE-CH") || 0==strcmp(ulang, "FR-CH") )
        M_df = 3;
    else
        M_df = 0;

    // amount format

    if ( 0==strcmp(ulang, "EN-US") || 0==strcmp(ulang, "EN-GB") || 0==strcmp(ulang, "EN-AU") || 0==strcmp(ulang, "TH-TH") )
    {
        M_tsep = ',';
        M_dsep = '.';
    }
    else if ( 0==strcmp(ulang, "PL-PL") || 0==strcmp(ulang, "IT-IT") || 0==strcmp(ulang, "NB-NO") || 0==strcmp(ulang, "ES-ES") )
    {
        M_tsep = '.';
        M_dsep = ',';
    }
    else
    {
        M_tsep = ' ';
        M_dsep = ',';
    }
}


/* --------------------------------------------------------------------------
   Format amount
---------------------------------------------------------------------------*/
void amt(char *stramt, long long in_amt)
{
    char    in_stramt[256];
    int     len;
    int     i, j=0;
    bool    minus=FALSE;

    sprintf(in_stramt, "%lld", in_amt);

    if ( in_stramt[0] == '-' )  /* change to proper UTF-8 minus sign */
    {
        strcpy(stramt, "− ");
        j = 4;
        minus = TRUE;
    }

    len = strlen(in_stramt);

/*  DBG("----- len = %d", len); */

    for ( i=(j?1:0); i<len; ++i, ++j )
    {
        if ( ((!minus && i) || (minus && i>1)) && !((len-i)%3) )
            stramt[j++] = M_tsep;
        stramt[j] = in_stramt[i];
    }

    stramt[j] = EOS;
}


/* --------------------------------------------------------------------------
   Format double amount
---------------------------------------------------------------------------*/
void amtd(char *stramt, double in_amt)
{
    char    in_stramt[64];
    int     len;
    int     i, j=0;
    bool    minus=FALSE;

    sprintf(in_stramt, "%0.2lf", in_amt);

    if ( in_stramt[0] == '-' )  /* change to proper UTF-8 minus sign */
    {
        strcpy(stramt, "− ");
        j = 4;
        minus = TRUE;
    }

    len = strlen(in_stramt);

    for ( i=(j?1:0); i<len; ++i, ++j )
    {
        if ( in_stramt[i]=='.' && M_dsep!='.' )
        {
            stramt[j] = M_dsep;
            continue;
        }
        else if ( ((!minus && i) || (minus && i>1)) && !((len-i)%3) && len-i > 3 && in_stramt[i] != ' ' && in_stramt[i-1] != ' ' && in_stramt[i-1] != '-' )
        {
            stramt[j++] = M_tsep;   /* extra character */
        }
        stramt[j] = in_stramt[i];
    }

    stramt[j] = EOS;
}


/* --------------------------------------------------------------------------
   Format amount -- special version
---------------------------------------------------------------------------*/
void lib_amt(char *stramt, long in_amt)
{
    char    in_stramt[64];
    int     len;
    int     i, j=0;
    bool    minus=FALSE;

    sprintf(in_stramt, "%ld", in_amt);

    if ( in_stramt[0] == '-' )
    {
        strcpy(stramt, "- ");
        j = 2;
        minus = TRUE;
    }

    len = strlen(in_stramt);

    for ( i=(j?1:0); i<len; ++i, ++j )
    {
        if ( ((!minus && i) || (minus && i>1)) && !((len-i)%3) )
            stramt[j++] = M_tsep;
        stramt[j] = in_stramt[i];
    }

    stramt[j] = EOS;
}


/* --------------------------------------------------------------------------
   Format double amount -- special version
---------------------------------------------------------------------------*/
void lib_amtd(char *stramt, double in_amt)
{
    char    in_stramt[64];
    int     len;
    int     i, j=0;
    bool    minus=FALSE;

    sprintf(in_stramt, "%0.2lf", in_amt);

    if ( in_stramt[0] == '-' )
    {
        strcpy(stramt, "- ");
        j = 2;
        minus = TRUE;
    }

    len = strlen(in_stramt);

    for ( i=(j?1:0); i<len; ++i, ++j )
    {
        if ( in_stramt[i]=='.' && M_dsep!='.' )
        {
            stramt[j] = M_dsep;
            continue;
        }
        else if ( ((!minus && i) || (minus && i>1)) && !((len-i)%3) && len-i > 3 && in_stramt[i] != ' ' && in_stramt[i-1] != ' ' && in_stramt[i-1] != '-' )
        {
            stramt[j++] = M_tsep;   /* extra character */
        }
        stramt[j] = in_stramt[i];
    }

    stramt[j] = EOS;
}


/* --------------------------------------------------------------------------
   Format double amount string to string
---------------------------------------------------------------------------*/
void samts(char *stramt, const char *in_amt)
{
    double  d;

    sscanf(in_amt, "%lf", &d);
    amtd(stramt, d);
}


/* --------------------------------------------------------------------------
   Convert string replacing first comma to dot
---------------------------------------------------------------------------*/
void lib_normalize_float(char *str)
{
    char *comma = strchr(str, ',');
    if ( comma ) *comma = '.';
}


/* --------------------------------------------------------------------------
   Format time (add separators between parts)
---------------------------------------------------------------------------*/
void ftm(char *strtm, long in_tm)
{
    char    in_strtm[16];
    int     i, j=0;
const char  sep=':';

    sprintf(in_strtm, "%06ld", in_tm);

    for ( i=0; i<6; ++i, ++j )
    {
        if ( i == 2 || i == 4 )
            strtm[j++] = sep;
        strtm[j] = in_strtm[i];
    }

    strtm[j] = EOS;
}


/* --------------------------------------------------------------------------
   Format date depending on M_df
---------------------------------------------------------------------------*/
char *fmt_date(short year, short month, short day)
{
static char date[16];

    if ( M_df == 1 )
        sprintf(date, "%02d/%02d/%d", month, day, year);
    else if ( M_df == 2 )
        sprintf(date, "%02d/%02d/%d", day, month, year);
    else if ( M_df == 3 )
        sprintf(date, "%02d.%02d.%d", day, month, year);
    else    /* M_df == 0 */
        sprintf(date, "%d-%02d-%02d", year, month, day);

    return date;
}


/* --------------------------------------------------------------------------
   SQL-escape string
-------------------------------------------------------------------------- */
char *silgy_sql_esc(const char *str)
{
static char dst[MAX_LONG_URI_VAL_LEN+1];
    int     i=0, j=0;

    while ( str[i] )
    {
        if ( j > MAX_LONG_URI_VAL_LEN-3 )
            break;
        else if ( str[i] == '\'' )
        {
            dst[j++] = '\\';
            dst[j++] = '\'';
        }
        else if ( str[i] == '"' )
        {
            dst[j++] = '\\';
            dst[j++] = '"';
        }
        else if ( str[i] == '\\' )
        {
            dst[j++] = '\\';
            dst[j++] = '\\';
        }
        else
            dst[j++] = str[i];
        ++i;
    }

    dst[j] = EOS;

    return dst;
}


/* --------------------------------------------------------------------------
   HTML-escape string
-------------------------------------------------------------------------- */
char *silgy_html_esc(const char *str)
{
static char dst[MAX_LONG_URI_VAL_LEN+1];
    int     i=0, j=0;

    while ( str[i] )
    {
        if ( j > MAX_LONG_URI_VAL_LEN-7 )
            break;
        else if ( str[i] == '\'' )
        {
            dst[j++] = '&';
            dst[j++] = 'a';
            dst[j++] = 'p';
            dst[j++] = 'o';
            dst[j++] = 's';
            dst[j++] = ';';
        }
        else if ( str[i] == '\\' )
        {
            dst[j++] = '\\';
            dst[j++] = '\\';
        }
        else if ( str[i] == '"' )
        {
            dst[j++] = '&';
            dst[j++] = 'q';
            dst[j++] = 'u';
            dst[j++] = 'o';
            dst[j++] = 't';
            dst[j++] = ';';
        }
        else if ( str[i] == '<' )
        {
            dst[j++] = '&';
            dst[j++] = 'l';
            dst[j++] = 't';
            dst[j++] = ';';
        }
        else if ( str[i] == '>' )
        {
            dst[j++] = '&';
            dst[j++] = 'g';
            dst[j++] = 't';
            dst[j++] = ';';
        }
        else if ( str[i] == '&' )
        {
            dst[j++] = '&';
            dst[j++] = 'a';
            dst[j++] = 'm';
            dst[j++] = 'p';
            dst[j++] = ';';
        }
        else if ( str[i] == '\n' )
        {
            dst[j++] = '<';
            dst[j++] = 'b';
            dst[j++] = 'r';
            dst[j++] = '>';
        }
        else if ( str[i] != '\r' )
            dst[j++] = str[i];
        ++i;
    }

    dst[j] = EOS;

    return dst;
}


/* --------------------------------------------------------------------------
   SQL-escape string respecting destination length (excluding '\0')
-------------------------------------------------------------------------- */
void sanitize_sql(char *dest, const char *str, int len)
{
    strncpy(dest, silgy_sql_esc(str), len);
    dest[len] = EOS;

    /* cut off orphaned single backslash */

    int i=len-1;
    int bs=0;
    while ( dest[i]=='\\' && i>-1 )
    {
        ++bs;
        i--;
    }

    if ( bs % 2 )   /* odd number of trailing backslashes -- cut one */
        dest[len-1] = EOS;
}


/* --------------------------------------------------------------------------
   ex unsan_noparse
   HTML un-escape string
-------------------------------------------------------------------------- */
char *silgy_html_unesc(const char *str)
{
static char dst[MAX_LONG_URI_VAL_LEN+1];
    int     i=0, j=0;

    while ( str[i] )
    {
        if ( j > MAX_LONG_URI_VAL_LEN-1 )
            break;
        else if ( i > 4
                    && str[i-5]=='&'
                    && str[i-4]=='a'
                    && str[i-3]=='p'
                    && str[i-2]=='o'
                    && str[i-1]=='s'
                    && str[i]==';' )
        {
            j -= 5;
            dst[j++] = '\'';
        }
        else if ( i > 1
                    && str[i-1]=='\\'
                    && str[i]=='\\' )
        {
            j -= 1;
            dst[j++] = '\\';
        }
        else if ( i > 4
                    && str[i-5]=='&'
                    && str[i-4]=='q'
                    && str[i-3]=='u'
                    && str[i-2]=='o'
                    && str[i-1]=='t'
                    && str[i]==';' )
        {
            j -= 5;
            dst[j++] = '"';
        }
        else if ( i > 2
                    && str[i-3]=='&'
                    && str[i-2]=='l'
                    && str[i-1]=='t'
                    && str[i]==';' )
        {
            j -= 3;
            dst[j++] = '<';
        }
        else if ( i > 2
                    && str[i-3]=='&'
                    && str[i-2]=='g'
                    && str[i-1]=='t'
                    && str[i]==';' )
        {
            j -= 3;
            dst[j++] = '>';
        }
        else if ( i > 3
                    && str[i-4]=='&'
                    && str[i-3]=='a'
                    && str[i-2]=='m'
                    && str[i-1]=='p'
                    && str[i]==';' )
        {
            j -= 4;
            dst[j++] = '&';
        }
        else if ( i > 2
                    && str[i-3]=='<'
                    && str[i-2]=='b'
                    && str[i-1]=='r'
                    && str[i]=='>' )
        {
            j -= 3;
            dst[j++] = '\n';
        }
        else
            dst[j++] = str[i];

        ++i;
    }

    dst[j] = EOS;

    return dst;
}


/* --------------------------------------------------------------------------
   Primitive URI encoding
---------------------------------------------------------------------------*/
/*char *uri_encode(const char *str)
{
static char uri_encode[4096];
    int     i;

    for ( i=0; str[i] && i<4095; ++i )
    {
        if ( str[i] == ' ' )
            uri_encode[i] = '+';
        else
            uri_encode[i] = str[i];
    }

    uri_encode[i] = EOS;

    return uri_encode;
}*/


/* --------------------------------------------------------------------------
   Convert string to upper
---------------------------------------------------------------------------*/
char *upper(const char *str)
{
static char upper[4096];
    int     i;

    for ( i=0; str[i] && i<4095; ++i )
    {
        if ( str[i] >= 97 && str[i] <= 122 )
            upper[i] = str[i] - 32;
        else
            upper[i] = str[i];
    }

    upper[i] = EOS;

    return upper;
}


/* --------------------------------------------------------------------------
   Strip trailing spaces from string
-------------------------------------------------------------------------- */
char *stp_right(char *str)
{
    char *p;

    for ( p = str + strlen(str) - 1;
          p >= str && (*p == ' ' || *p == '\t');
          p-- )
          *p = 0;

    return str;
}


/* --------------------------------------------------------------------------
   Return TRUE if digits only
---------------------------------------------------------------------------*/
bool strdigits(const char *src)
{
    int i;

    for ( i=0; src[i]; ++i )
    {
        if ( !isdigit(src[i]) )
            return FALSE;
    }

    return TRUE;
}


/* --------------------------------------------------------------------------
   Copy string without spaces and tabs
---------------------------------------------------------------------------*/
char *nospaces(char *dst, const char *src)
{
    const char  *p=src;
    int     i=0;

    while ( *p )
    {
        if ( *p != ' ' && *p != '\t' )
            dst[i++] = *p;
        ++p;
    }

    dst[i] = EOS;

    return dst;
}


/* --------------------------------------------------------------------------
   Return a random 8-bit number from M_random_numbers
-------------------------------------------------------------------------- */
static unsigned char get_random_number()
{
    static int i=0;

    if ( M_random_initialized )
    {
        if ( i >= RANDOM_NUMBERS )  /* refill the pool with fresh numbers */
        {
            init_random_numbers();
            i = 0;
        }
        return M_random_numbers[i++];
    }
    else
    {
        WAR("Using get_random_number() before M_random_initialized");
        return rand() % 256;
    }
}


/* --------------------------------------------------------------------------
   Seed rand()
-------------------------------------------------------------------------- */
static void seed_rand()
{
#define SILGY_SEEDS_MEM 256  /* remaining 8 bits & last seeds to remember */
static char first=1;
/* make sure at least the last SILGY_SEEDS_MEM seeds are unique */
static unsigned int seeds[SILGY_SEEDS_MEM];

    DBG("seed_rand");

    /* generate possibly random, or at least based on some non-deterministic factors, 32-bit integer */

    int time_remainder = (int)G_now % 63 + 1;     /* 6 bits */
    int mem_remainder = lib_get_memory() % 63 + 1;    /* 6 bits */
    int pid_remainder;       /* 6 bits */
    int yesterday_rem;    /* 6 bits */

    if ( first )    /* initial seed */
    {
        pid_remainder = G_pid % 63 + 1;
        yesterday_rem = G_cnts_yesterday.req % 63 + 1;
    }
    else    /* subsequent calls */
    {
        pid_remainder = get_random_number() % 63 + 1;
        yesterday_rem = get_random_number() % 63 + 1;
    }

    static int seeded=0;    /* 8 bits */

    unsigned int seed;
static unsigned int prev_seed=0;

    while ( 1 )
    {
        if ( seeded >= SILGY_SEEDS_MEM )
            seeded = 1;
        else
            ++seeded;

        seed = pid_remainder * time_remainder * mem_remainder * yesterday_rem * seeded;

        /* check uniqueness in the history */

        char found=0;
        int i = 0;
        while ( i < SILGY_SEEDS_MEM )
        {
            if ( seeds[i++] == seed )
            {
                found = 1;
                break;
            }
        }

        if ( found )    /* same seed again */
        {
            WAR("seed %u repeated; seeded = %d, i = %d", seed, seeded, i);
        }
        else   /* seed not found ==> OK */
        {
            /* new seed needs to be at least 10000 apart from the previous one */

            if ( !first && abs((long long)(seed-prev_seed)) < 10000 )
            {
                WAR("seed %u too close to the previous one (%u); seeded = %d", seed, prev_seed, seeded);
            }
            else    /* everything OK */
            {
                seeds[seeded-1] = seed;
                break;
            }
        }

        /* stir it up to avoid endless loop */

        pid_remainder = get_random_number() % 63 + 1;
        time_remainder = get_random_number() % 63 + 1;
    }

    char f[256];
    amt(f, seed);
    DBG("seed = %s", f);
    DBG("");

    prev_seed = seed;

    first = 0;

    srand(seed);
}


/* --------------------------------------------------------------------------
   Initialize M_random_numbers array
-------------------------------------------------------------------------- */
void init_random_numbers()
{
    int i;

#ifdef DUMP
    struct timespec start;
#ifdef _WIN32
    clock_gettime_win(&start);
#else
    clock_gettime(MONOTONIC_CLOCK_NAME, &start);
#endif
#endif  /* DUMP */

    DBG("init_random_numbers");

    seed_rand();

#ifdef __linux__
    /* On Linux we have access to a hardware-influenced RNG */

    DBG("Trying /dev/urandom...");

    int urandom_fd = open("/dev/urandom", O_RDONLY);

    if ( urandom_fd )
    {
        read(urandom_fd, &M_random_numbers, RANDOM_NUMBERS);

        close(urandom_fd);

        INF("M_random_numbers obtained from /dev/urandom");
    }
    else
    {
        WAR("Couldn't open /dev/urandom");

        /* fallback to old plain rand(), seeded the best we could */

        for ( i=0; i<RANDOM_NUMBERS; ++i )
            M_random_numbers[i] = rand() % 256;

        INF("M_random_numbers obtained from rand()");
    }

#else   /* Windows */

    for ( i=0; i<RANDOM_NUMBERS; ++i )
        M_random_numbers[i] = rand() % 256;

    INF("M_random_numbers obtained from rand()");

#endif

    INF("");

    M_random_initialized = 1;

#ifdef DUMP
    DBG("--------------------------------------------------------------------------------------------------------------------------------");
    DBG("M_random_numbers distribution visualization");
    DBG("The square below should be filled fairly randomly and uniformly.");
    DBG("If it's not, or you can see any regular patterns across the square, your system may be broken or too old to be deemed secure.");
    DBG("--------------------------------------------------------------------------------------------------------------------------------");

    /* One square takes two columns, so we can have between 0 and 4 dots per square */

#define SQUARE_ROWS             64
#define SQUARE_COLS             SQUARE_ROWS*2
#define SQUARE_IS_EMPTY(x, y)   (dots[y][x*2]==' ' && dots[y][x*2+1]==' ')
#define SQUARE_HAS_ONE(x, y)    (dots[y][x*2]==' ' && dots[y][x*2+1]=='.')
#define SQUARE_HAS_TWO(x, y)    (dots[y][x*2]=='.' && dots[y][x*2+1]=='.')
#define SQUARE_HAS_THREE(x, y)  (dots[y][x*2]=='.' && dots[y][x*2+1]==':')
#define SQUARE_HAS_FOUR(x, y)   (dots[y][x*2]==':' && dots[y][x*2+1]==':')

    char dots[SQUARE_ROWS][SQUARE_COLS+1]={0};
    int j;

    for ( i=0; i<SQUARE_ROWS; ++i )
        for ( j=0; j<SQUARE_COLS; ++j )
            dots[i][j] = ' ';

    /* we only have SQUARE_ROWS^2 squares with 5 possible values in each of them */
    /* let only once in divider in */

    int divider = RANDOM_NUMBERS / (SQUARE_ROWS*SQUARE_ROWS) + 1;

    for ( i=0; i<RANDOM_NUMBERS-1; i+=2 )
    {
        if ( i % divider ) continue;

        int x = M_random_numbers[i] % SQUARE_ROWS;
        int y = M_random_numbers[i+1] % SQUARE_ROWS;

        if ( SQUARE_IS_EMPTY(x, y) )    /* make it one */
            dots[y][x*2+1] = '.';
        else if ( SQUARE_HAS_ONE(x, y) )    /* make it two */
            dots[y][x*2] = '.';
        else if ( SQUARE_HAS_TWO(x, y) )    /* make it three */
            dots[y][x*2+1] = ':';
        else if ( SQUARE_HAS_THREE(x, y) )  /* make it four */
            dots[y][x*2] = ':';
    }

    for ( i=0; i<SQUARE_ROWS; ++i )
        DBG(dots[i]);

    DBG("--------------------------------------------------------------------------------------------------------------------------------");
    DBG("");
    DBG("init_random_numbers took %.3lf ms", lib_elapsed(&start));
    DBG("");
#endif
}


/* --------------------------------------------------------------------------
   Generate random string
   Generates FIPS-compliant random sequences (tested with Burp)
-------------------------------------------------------------------------- */
void silgy_random(char *dest, int len)
{
const char  *chars="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static int  since_seed=0;
    int     i;

#ifdef DUMP
    struct timespec start;
#ifdef _WIN32
    clock_gettime_win(&start);
#else
    clock_gettime(MONOTONIC_CLOCK_NAME, &start);
#endif
#endif  /* DUMP */

    if ( since_seed > (G_cnts_today.req % 246 + 10) )  /* seed every now and then */
//    if ( 1 )  /* test */
    {
        seed_rand();
        since_seed = 0;
    }
    else
    {
        ++since_seed;
#ifdef DUMP
        DBG("since_seed = %d", since_seed);
#endif
    }

#ifdef DUMP
    DBG_LINE;
#endif

    int r;

    for ( i=0; i<len; ++i )
    {
        /* source random numbers from two different sets: 'normal' and 'lucky' */

        if ( get_random_number() % 3 == 0 )
        {
#ifdef DUMP
            DBG("i=%d lucky", i);
#endif
            r = get_random_number();
            while ( r > 247 ) r = get_random_number();   /* avoid modulo bias -- 62*4 - 1 */
        }
        else
        {
#ifdef DUMP
            DBG("i=%d normal", i);
#endif
            r = rand() % 256;
            while ( r > 247 ) r = rand() % 256;
        }

        dest[i] = chars[r % 62];
    }

    dest[i] = EOS;

#ifdef DUMP
    DBG_LINE;
#endif

#ifdef DUMP
    DBG("silgy_random took %.3lf ms", lib_elapsed(&start));
#endif
}


/* --------------------------------------------------------------------------
   Sleep for n miliseconds
   n must be less than 1 second (< 1000)!
-------------------------------------------------------------------------- */
void msleep(int msec)
{
    struct timeval tv;

    if ( msec < 1000 )
    {
        tv.tv_sec = 0;
        tv.tv_usec = msec*1000;
    }
    else    /* 1000 ms or more */
    {
        tv.tv_sec = msec/1000;
        tv.tv_usec = (msec-((int)(msec/1000)*1000))*1000;
    }

/*    DBG("-----------------------");
    DBG("msec = %d", msec);
    DBG("tv.tv_sec = %d", tv.tv_sec);
    DBG("tv.tv_usec = %d", tv.tv_usec); */

    select(0, NULL, NULL, NULL, &tv);
}


#ifdef AUTO_INIT_EXPERIMENT
/* --------------------------------------------------------------------------
   Implicitly init JSON buffer
-------------------------------------------------------------------------- */
static void json_auto_init(JSON *json)
{
    int     i;
    bool    initialized=0;

    for ( i=0; i<M_jsons_cnt; ++i )
    {
        if ( M_jsons[i] == json )
        {
            initialized = 1;
            break;
        }
    }

    if ( !initialized )     /* recognize it by the address */
    {
        if ( M_jsons_cnt >= JSON_MAX_JSONS )
            M_jsons_cnt = 0;

        M_jsons[M_jsons_cnt] = json;
        ++M_jsons_cnt;
        lib_json_reset(json);
    }
}
#endif  /* AUTO_INIT_EXPERIMENT */


/* --------------------------------------------------------------------------
   Reset JSON object
-------------------------------------------------------------------------- */
void lib_json_reset(JSON *json)
{
#ifdef DUMP
    DBG("lib_json_reset");
#endif
    json->cnt = 0;
    json->array = 0;
}


/* --------------------------------------------------------------------------
   Get index from JSON buffer
-------------------------------------------------------------------------- */
static int json_get_i(JSON *json, const char *name)
{
    int     i;

    for ( i=0; i<json->cnt; ++i )
        if ( 0==strcmp(json->rec[i].name, name) )
            return i;

    return -1;
}


/* --------------------------------------------------------------------------
   Convert Silgy JSON format to JSON string
   Reentrant version
-------------------------------------------------------------------------- */
static void json_to_string(char *dst, JSON *json, bool array)
{
    char    *p=dst;
    int     i;

    p = stpcpy(p, array?"[":"{");

    for ( i=0; i<json->cnt; ++i )
    {
        /* key */

        if ( !array )
        {
            p = stpcpy(p, "\"");
            p = stpcpy(p, json->rec[i].name);
            p = stpcpy(p, "\":");
        }

        /* value */

        if ( json->rec[i].type == JSON_STRING )
        {
            p = stpcpy(p, "\"");
            p = stpcpy(p, json->rec[i].value);
            p = stpcpy(p, "\"");
        }
        else if ( json->rec[i].type==JSON_INTEGER || json->rec[i].type==JSON_FLOAT || json->rec[i].type==JSON_BOOL )
        {
            p = stpcpy(p, json->rec[i].value);
        }
        else if ( json->rec[i].type == JSON_RECORD )
        {
            char tmp[JSON_BUFSIZE];
            json_to_string(tmp, (JSON*)atol(json->rec[i].value), FALSE);
            p = stpcpy(p, tmp);
        }
        else if ( json->rec[i].type == JSON_ARRAY )
        {
            char tmp[JSON_BUFSIZE];
            json_to_string(tmp, (JSON*)atol(json->rec[i].value), TRUE);
            p = stpcpy(p, tmp);
        }

        if ( i < json->cnt-1 )
            p = stpcpy(p, ",");
    }

    p = stpcpy(p, array?"]":"}");

    *p = EOS;
}


/* --------------------------------------------------------------------------
   Add indent
-------------------------------------------------------------------------- */
static char *json_indent(int level)
{
#define JSON_PRETTY_INDENT "    "

static char dst[4096];
    int     i;

    dst[0] = EOS;

    for ( i=0; i<level; ++i )
        strcat(dst, JSON_PRETTY_INDENT);

    return dst;
}


/* --------------------------------------------------------------------------
   Convert Silgy JSON format to JSON string
   Reentrant version
-------------------------------------------------------------------------- */
static void json_to_string_pretty(char *dst, JSON *json, bool array, int level)
{
    char    *p=dst;
    int     i;

    p = stpcpy(p, array?"[\n":"{\n");

    for ( i=0; i<json->cnt; ++i )
    {
        p = stpcpy(p, json_indent(level));

        /* key */

        if ( !array )
        {
            p = stpcpy(p, "\"");
            p = stpcpy(p, json->rec[i].name);
            p = stpcpy(p, "\": ");
        }

        /* value */

        if ( json->rec[i].type == JSON_STRING )
        {
            p = stpcpy(p, "\"");
            p = stpcpy(p, json->rec[i].value);
            p = stpcpy(p, "\"");
        }
        else if ( json->rec[i].type==JSON_INTEGER || json->rec[i].type==JSON_FLOAT || json->rec[i].type==JSON_BOOL )
        {
            p = stpcpy(p, json->rec[i].value);
        }
        else if ( json->rec[i].type == JSON_RECORD )
        {
            if ( !array || i > 0 )
            {
                p = stpcpy(p, "\n");
                p = stpcpy(p, json_indent(level));
            }
            char tmp[JSON_BUFSIZE];
            json_to_string_pretty(tmp, (JSON*)atol(json->rec[i].value), FALSE, level+1);
            p = stpcpy(p, tmp);
        }
        else if ( json->rec[i].type == JSON_ARRAY )
        {
            if ( !array || i > 0 )
            {
                p = stpcpy(p, "\n");
                p = stpcpy(p, json_indent(level));
            }
            char tmp[JSON_BUFSIZE];
            json_to_string_pretty(tmp, (JSON*)atol(json->rec[i].value), TRUE, level+1);
            p = stpcpy(p, tmp);
        }

        if ( i < json->cnt-1 )
            p = stpcpy(p, ",");

        p = stpcpy(p, "\n");
    }

    p = stpcpy(p, json_indent(level-1));
    p = stpcpy(p, array?"]":"}");

    *p = EOS;
}


/* --------------------------------------------------------------------------
   Convert Silgy JSON format to JSON string
-------------------------------------------------------------------------- */
char *lib_json_to_string(JSON *json)
{
static char dst[JSON_BUFSIZE];

    json_to_string(dst, json, FALSE);

    return dst;
}


/* --------------------------------------------------------------------------
   Convert Silgy JSON format to JSON string
-------------------------------------------------------------------------- */
char *lib_json_to_string_pretty(JSON *json)
{
static char dst[JSON_BUFSIZE];

    json_to_string_pretty(dst, json, FALSE, 1);

    return dst;
}


/* --------------------------------------------------------------------------
   Find matching closing bracket in JSON string
-------------------------------------------------------------------------- */
static char *get_json_closing_bracket(const char *src)
{
    int     i=1, subs=0;
    bool    in_quotes=0;

#ifdef DUMP
//    DBG("get_json_closing_bracket [%s]", src);
#endif  /* DUMP */

    while ( src[i] )
    {
        if ( src[i]=='"' )
        {
            if ( in_quotes )
                in_quotes = 0;
            else
                in_quotes = 1;
        }
        else if ( src[i]=='{' && !in_quotes )
        {
            ++subs;
        }
        else if ( src[i]=='}' && !in_quotes )
        {
            if ( subs<1 )
                return (char*)src+i;
            else
                subs--;
        }

        ++i;
    }

    return NULL;
}


/* --------------------------------------------------------------------------
   Find matching closing square bracket in JSON string
-------------------------------------------------------------------------- */
static char *get_json_closing_square_bracket(const char *src)
{
    int     i=1, subs=0;
    bool    in_quotes=0;

#ifdef DUMP
//    DBG("get_json_closing_square_bracket [%s]", src);
#endif  /* DUMP */

    while ( src[i] )
    {
        if ( src[i]=='"' )
        {
            if ( in_quotes )
                in_quotes = 0;
            else
                in_quotes = 1;
        }
        else if ( src[i]=='[' && !in_quotes )
        {
            ++subs;
        }
        else if ( src[i]==']' && !in_quotes )
        {
            if ( subs<1 )
                return (char*)src+i;
            else
                subs--;
        }

        ++i;
    }

    return NULL;
}


/* --------------------------------------------------------------------------
   Convert JSON string to Silgy JSON format
-------------------------------------------------------------------------- */
bool lib_json_from_string(JSON *json, const char *src, int len, int level)
{
    int     i=0, j=0;
    char    key[JSON_KEY_LEN+1];
    char    value[JSON_VAL_LEN+1];
    int     index;
    char    now_key=0, now_value=0, inside_array=0, type;

static JSON json_pool[JSON_POOL_SIZE*JSON_MAX_LEVELS];
static int  json_pool_cnt[JSON_MAX_LEVELS]={0};

    if ( len == 0 ) len = strlen(src);

    if ( level == 0 )
    {
        lib_json_reset(json);

        while ( i<len && src[i] != '{' ) ++i;   /* skip junk if there's any */

        if ( src[i] != '{' )    /* no opening bracket */
        {
            ERR("JSON syntax error -- no opening curly bracket");
            return FALSE;
        }

        ++i;    /* skip '{' */
    }
    else if ( src[i]=='{' )     /* record */
    {
        ++i;    /* skip '{' */
    }
    else if ( src[i]=='[' )     /* array */
    {
        inside_array = 1;
        ++i;    /* skip '[' */
        index = -1;
    }

#ifdef DUMP
static char tmp[JSON_BUFSIZE];
    strncpy(tmp, src+i, len-i);
    tmp[len-i] = EOS;
    char debug[64];
    sprintf(debug, "lib_json_from_string level %d", level);
    log_long(tmp, len, debug);
    if ( inside_array ) DBG("inside_array");
#endif  /* DUMP */

    for ( i; i<len; ++i )
    {
        if ( !now_key && !now_value )
        {
            while ( i<len && (src[i]==' ' || src[i]=='\t' || src[i]=='\r' || src[i]=='\n') ) ++i;

            if ( !inside_array && src[i]=='"' )  /* start of key */
            {
                now_key = 1;
                j = 0;
                ++i;    /* skip '"' */
            }
        }

        if ( (now_key && src[i]=='"') || (inside_array && !now_value && (index==-1 || src[i]==',')) )      /* end of key */
        {
#ifdef DUMP
            if ( now_key && src[i]=='"' )
                DBG("second if because of now_key");
            else
                DBG("second if because of inside_array");
#endif  /* DUMP */
            if ( inside_array )
            {
                if ( src[i]==',' ) ++i;    /* skip ',' */

                ++index;
#ifdef DUMP
                DBG("inside_array, starting new value, index = %d", index);
#endif
            }
            else
            {
                key[j] = EOS;
#ifdef DUMP
                DBG("key [%s]", key);
#endif
                now_key = 0;

                ++i;    /* skip '"' */

                while ( i<len && src[i]!=':' ) ++i;

                if ( src[i] != ':' )
                {
                    ERR("JSON syntax error -- no colon after name");
                    return FALSE;
                }

                ++i;    /* skip ':' */
            }

            while ( i<len && (src[i]==' ' || src[i]=='\t' || src[i]=='\r' || src[i]=='\n') ) ++i;

            if ( i==len )
            {
                ERR("JSON syntax error -- expected value");
                return FALSE;
            }

            /* value starts here --------------------------------------------------- */

            if ( src[i]=='"' )    /* JSON_STRING */
            {
#ifdef DUMP
                DBG("JSON_STRING");
#endif
                type = JSON_STRING;

                now_value = 1;
                j = 0;
            }
            else if ( src[i]=='{' )     /* JSON_RECORD */
            {
#ifdef DUMP
                DBG("JSON_RECORD");
#endif
                type = JSON_RECORD;

                if ( level < JSON_MAX_LEVELS-1 )
                {
                    if ( json_pool_cnt[level] >= JSON_POOL_SIZE ) json_pool_cnt[level] = 0;   /* overwrite previous ones */

                    int pool_idx = JSON_POOL_SIZE*level + json_pool_cnt[level];
                    lib_json_reset(&json_pool[pool_idx]);
                    /* save the pointer first as a parent record */
                    if ( inside_array )
                        lib_json_add_record(json, NULL, &json_pool[pool_idx], FALSE, index);
                    else
                        lib_json_add_record(json, key, &json_pool[pool_idx], FALSE, -1);
                    /* fill in the destination (children) */
                    char *closing;
                    if ( (closing=get_json_closing_bracket(src+i)) )
                    {
//                        DBG("closing [%s], len=%d", closing, closing-(src+i));
                        lib_json_from_string(&json_pool[pool_idx], src+i, closing-(src+i)+1, level+1);
                        ++json_pool_cnt[level];
                        i += closing-(src+i);
//                        DBG("after closing record bracket [%s]", src+i);
                    }
                    else    /* syntax error */
                    {
                        ERR("No closing bracket in JSON record");
                        break;
                    }
                }
            }
            else if ( src[i]=='[' )     /* JSON_ARRAY */
            {
#ifdef DUMP
                DBG("JSON_ARRAY");
#endif
                type = JSON_ARRAY;

                if ( level < JSON_MAX_LEVELS-1 )
                {
                    if ( json_pool_cnt[level] >= JSON_POOL_SIZE ) json_pool_cnt[level] = 0;   /* overwrite previous ones */

                    int pool_idx = JSON_POOL_SIZE*level + json_pool_cnt[level];
                    lib_json_reset(&json_pool[pool_idx]);
                    /* save the pointer first as a parent record */
                    if ( inside_array )
                        lib_json_add_record(json, NULL, &json_pool[pool_idx], TRUE, index);
                    else
                        lib_json_add_record(json, key, &json_pool[pool_idx], TRUE, -1);
                    /* fill in the destination (children) */
                    char *closing;
                    if ( (closing=get_json_closing_square_bracket(src+i)) )
                    {
//                        DBG("closing [%s], len=%d", closing, closing-(src+i));
                        lib_json_from_string(&json_pool[pool_idx], src+i, closing-(src+i)+1, level+1);
                        ++json_pool_cnt[level];
                        i += closing-(src+i);
//                        DBG("after closing array bracket [%s]", src+i);
                    }
                    else    /* syntax error */
                    {
                        ERR("No closing square bracket in JSON array");
                        break;
                    }
                }
            }
            else    /* number */
            {
#ifdef DUMP
                DBG("JSON_INTEGER || JSON_FLOAT || JSON_BOOL");
#endif
                type = JSON_INTEGER;    /* we're not sure yet but need to mark it's definitely not STRING */

                i--;

                now_value = 1;
                j = 0;
            }
        }
        else if ( now_value && ((type==JSON_STRING && src[i]=='"' && src[i-1]!='\\') || src[i]==',' || src[i]=='}' || src[i]==']' || src[i]=='\r' || src[i]=='\n') )     /* end of value */
        {
            value[j] = EOS;
#ifdef DUMP
            DBG("value [%s]", value);
#endif
            if ( type==JSON_STRING ) ++i;   /* skip closing '"' */

            /* src[i] should now be at ',' */

            if ( inside_array )
            {
                if ( type==JSON_STRING )
                    lib_json_add(json, NULL, value, 0, 0, JSON_STRING, index);
                else if ( value[0]=='t' )
                    lib_json_add(json, NULL, NULL, 1, 0, JSON_BOOL, index);
                else if ( value[0]=='f' )
                    lib_json_add(json, NULL, NULL, 0, 0, JSON_BOOL, index);
                else if ( strchr(value, '.') )
                    lib_json_add(json, NULL, NULL, 0, atof(value), JSON_FLOAT, index);
                else
                    lib_json_add(json, NULL, NULL, atol(value), 0, JSON_INTEGER, index);
            }
            else
            {
                if ( type==JSON_STRING )
                    lib_json_add(json, key, value, 0, 0, JSON_STRING, -1);
                else if ( value[0]=='t' )
                    lib_json_add(json, key, NULL, 1, 0, JSON_BOOL, -1);
                else if ( value[0]=='f' )
                    lib_json_add(json, key, NULL, 0, 0, JSON_BOOL, -1);
                else if ( strchr(value, '.') )
                    lib_json_add(json, key, NULL, 0, atof(value), JSON_FLOAT, -1);
                else
                    lib_json_add(json, key, NULL, atol(value), 0, JSON_INTEGER, -1);
            }

            now_value = 0;

            if ( src[i]==',' ) i--;     /* we need it to detect the next array element */
        }
        else if ( now_key )
        {
            if ( j < JSON_KEY_LEN )
                key[j++] = src[i];
        }
        else if ( now_value )
        {
            if ( j < JSON_VAL_LEN )
                value[j++] = src[i];
        }

//        if ( src[i-2]=='}' && !now_value && level==0 )
//            break;
    }

    return TRUE;
}


/* --------------------------------------------------------------------------
   Log JSON buffer
-------------------------------------------------------------------------- */
void lib_json_log_dbg(JSON *json, const char *name)
{
    int     i;
    char    type[32];

    DBG_LINE;

    if ( name )
        DBG("%s:", name);
    else
        DBG("JSON record:");

    for ( i=0; i<json->cnt; ++i )
    {
        if ( json->rec[i].type == JSON_STRING )
            strcpy(type, "JSON_STRING");
        else if ( json->rec[i].type == JSON_INTEGER )
            strcpy(type, "JSON_INTEGER");
        else if ( json->rec[i].type == JSON_FLOAT )
            strcpy(type, "JSON_FLOAT");
        else if ( json->rec[i].type == JSON_BOOL )
            strcpy(type, "JSON_BOOL");
        else if ( json->rec[i].type == JSON_RECORD )
            strcpy(type, "JSON_RECORD");
        else if ( json->rec[i].type == JSON_ARRAY )
            strcpy(type, "JSON_ARRAY");
        else
        {
            sprintf(type, "Unknown type! (%d)", json->rec[i].type);
            break;
        }

        DBG("%d %s [%s] %s", i, json->array?"":json->rec[i].name, json->rec[i].value, type);
    }

    DBG_LINE;
}


/* --------------------------------------------------------------------------
   Log JSON buffer
-------------------------------------------------------------------------- */
void lib_json_log_inf(JSON *json, const char *name)
{
    int     i;
    char    type[32];

    INF_LINE;

    if ( name )
        INF("%s:", name);
    else
        INF("JSON record:");

    for ( i=0; i<json->cnt; ++i )
    {
        if ( json->rec[i].type == JSON_STRING )
            strcpy(type, "JSON_STRING");
        else if ( json->rec[i].type == JSON_INTEGER )
            strcpy(type, "JSON_INTEGER");
        else if ( json->rec[i].type == JSON_FLOAT )
            strcpy(type, "JSON_FLOAT");
        else if ( json->rec[i].type == JSON_BOOL )
            strcpy(type, "JSON_BOOL");
        else if ( json->rec[i].type == JSON_RECORD )
            strcpy(type, "JSON_RECORD");
        else if ( json->rec[i].type == JSON_ARRAY )
            strcpy(type, "JSON_ARRAY");
        else
        {
            sprintf(type, "Unknown type! (%d)", json->rec[i].type);
            break;
        }

        INF("%d %s [%s] %s", i, json->array?"":json->rec[i].name, json->rec[i].value, type);
    }

    INF_LINE;
}


/* --------------------------------------------------------------------------
   Add/set value to a JSON buffer
-------------------------------------------------------------------------- */
bool lib_json_add(JSON *json, const char *name, const char *str_value, long int_value, double flo_value, char type, int i)
{
#ifdef AUTO_INIT_EXPERIMENT
    json_auto_init(json);
#endif

    if ( name )
    {
        i = json_get_i(json, name);

        if ( i==-1 )    /* not present -- append new */
        {
            if ( json->cnt >= JSON_MAX_ELEMS ) return FALSE;
            i = json->cnt;
            ++json->cnt;
            strncpy(json->rec[i].name, name, 31);
            json->rec[i].name[31] = EOS;
            json->array = FALSE;
        }
    }
    else    /* array */
    {
        if ( i >= JSON_MAX_ELEMS-1 ) return FALSE;
        json->array = TRUE;
        if ( json->cnt < i+1 ) json->cnt = i + 1;
    }

    if ( type == JSON_STRING )
    {
        strncpy(json->rec[i].value, str_value, JSON_VAL_LEN);
        json->rec[i].value[JSON_VAL_LEN] = EOS;
    }
    else if ( type == JSON_BOOL )
    {
        if ( int_value )
            strcpy(json->rec[i].value, "true");
        else
            strcpy(json->rec[i].value, "false");
    }
    else if ( type == JSON_INTEGER )
    {
        sprintf(json->rec[i].value, "%ld", int_value);
    }
    else    /* float */
    {
        snprintf(json->rec[i].value, 256, "%f", flo_value);
    }

    json->rec[i].type = type;

    return TRUE;
}


/* --------------------------------------------------------------------------
   Insert value into JSON buffer
-------------------------------------------------------------------------- */
bool lib_json_add_record(JSON *json, const char *name, JSON *json_sub, bool is_array, int i)
{
    DBG("lib_json_add_record (%s)", is_array?"ARRAY":"RECORD");

#ifdef AUTO_INIT_EXPERIMENT
    json_auto_init(json);
#endif

    if ( name )
    {
#ifdef DUMP
        DBG("name [%s]", name);
#endif
        i = json_get_i(json, name);

        if ( i==-1 )    /* not present -- append new */
        {
            if ( json->cnt >= JSON_MAX_ELEMS ) return FALSE;
            i = json->cnt;
            ++json->cnt;
            strncpy(json->rec[i].name, name, 31);
            json->rec[i].name[31] = EOS;
            json->array = FALSE;
        }
    }
    else    /* array */
    {
#ifdef DUMP
        DBG("index = %d", i);
#endif
        if ( i >= JSON_MAX_ELEMS-1 ) return FALSE;
        json->array = TRUE;
        if ( json->cnt < i+1 ) json->cnt = i + 1;
    }

    /* store sub-record address as a text in value */

    sprintf(json->rec[i].value, "%ld", (long)json_sub);

    json->rec[i].type = is_array?JSON_ARRAY:JSON_RECORD;

    return TRUE;
}


/* --------------------------------------------------------------------------
   Get value from JSON buffer
-------------------------------------------------------------------------- */
char *lib_json_get_str(JSON *json, const char *name, int i)
{
static char dst[JSON_VAL_LEN+1];

    if ( !name )    /* array elem */
    {
        if ( i >= json->cnt )
        {
            ERR("lib_json_get_str index (%d) out of bound (max = %d)", i, json->cnt-1);
            dst[0] = EOS;
            return dst;
        }

        if ( json->rec[i].type==JSON_STRING || json->rec[i].type==JSON_INTEGER || json->rec[i].type==JSON_FLOAT || json->rec[i].type==JSON_BOOL )
        {
            strcpy(dst, json->rec[i].value);
            return dst;
        }
        else    /* types don't match */
        {
            dst[0] = EOS;
            return dst;   /* types don't match or couldn't convert */
        }
    }

    for ( i=0; i<json->cnt; ++i )
    {
        if ( 0==strcmp(json->rec[i].name, name) )
        {
            if ( json->rec[i].type==JSON_STRING || json->rec[i].type==JSON_INTEGER || json->rec[i].type==JSON_FLOAT || json->rec[i].type==JSON_BOOL )
            {
                strcpy(dst, json->rec[i].value);
                return dst;
            }

            dst[0] = EOS;
            return dst;   /* types don't match or couldn't convert */
        }
    }

    dst[0] = EOS;
    return dst;   /* no such field */
}


/* --------------------------------------------------------------------------
   Get value from JSON buffer
-------------------------------------------------------------------------- */
long lib_json_get_int(JSON *json, const char *name, int i)
{
    if ( !name )    /* array elem */
    {
        if ( i >= json->cnt )
        {
            ERR("lib_json_get_int index (%d) out of bound (max = %d)", i, json->cnt-1);
            return 0;
        }

        if ( json->rec[i].type == JSON_INTEGER )
            return atol(json->rec[i].value);
        else    /* types don't match */
            return 0;
    }

    for ( i=0; i<json->cnt; ++i )
    {
        if ( 0==strcmp(json->rec[i].name, name) )
        {
            if ( json->rec[i].type == JSON_INTEGER )
            {
                return atol(json->rec[i].value);
            }

            return 0;   /* types don't match or couldn't convert */
        }
    }

    return 0;   /* no such field */
}


/* --------------------------------------------------------------------------
   Get value from JSON buffer
-------------------------------------------------------------------------- */
double lib_json_get_float(JSON *json, const char *name, int i)
{
    if ( !name )    /* array elem */
    {
        if ( i >= json->cnt )
        {
            ERR("lib_json_get_float index (%d) out of bound (max = %d)", i, json->cnt-1);
            return 0;
        }

        if ( json->rec[i].type == JSON_FLOAT )
            return atof(json->rec[i].value);
        else    /* types don't match */
            return 0;
    }

    for ( i=0; i<json->cnt; ++i )
    {
        if ( 0==strcmp(json->rec[i].name, name) )
        {
            if ( json->rec[i].type == JSON_FLOAT )
            {
                return atof(json->rec[i].value);
            }

            return 0;   /* types don't match or couldn't convert */
        }
    }

    return 0;   /* no such field */
}


/* --------------------------------------------------------------------------
   Get value from JSON buffer
-------------------------------------------------------------------------- */
bool lib_json_get_bool(JSON *json, const char *name, int i)
{
    if ( !name )    /* array elem */
    {
        if ( i >= json->cnt )
        {
            ERR("lib_json_get_bool index (%d) out of bound (max = %d)", i, json->cnt-1);
            return FALSE;
        }

        if ( json->rec[i].type == JSON_BOOL )
        {
            if ( json->rec[i].value[0] == 't' )
                return TRUE;
            else
                return FALSE;
        }
        else if ( json->rec[i].type == JSON_STRING )
        {
            if ( 0==strcmp(json->rec[i].value, "true") )
                return TRUE;
            else
                return FALSE;
        }
        else    /* types don't match */
        {
            return FALSE;
        }
    }

    for ( i=0; i<json->cnt; ++i )
    {
        if ( 0==strcmp(json->rec[i].name, name) )
        {
            if ( json->rec[i].type == JSON_BOOL )
            {
                if ( json->rec[i].value[0] == 't' )
                    return TRUE;
                else
                    return FALSE;
            }
            else if ( json->rec[i].type == JSON_STRING )
            {
                if ( 0==strcmp(json->rec[i].value, "true") )
                    return TRUE;
                else
                    return FALSE;
            }

            return FALSE;   /* types don't match or couldn't convert */
        }
    }

    return FALSE;   /* no such field */
}


/* --------------------------------------------------------------------------
   Get (copy) value from JSON buffer
   How to change it to returning pointer without confusing beginners?
   It would be better performing without copying all the fields
-------------------------------------------------------------------------- */
bool lib_json_get_record(JSON *json, const char *name, JSON *json_sub, int i)
{
    DBG("lib_json_get_record by %s", name?"name":"index");

    if ( !name )    /* array elem */
    {
        if ( i >= json->cnt )
        {
            ERR("lib_json_get_record index (%d) out of bound (max = %d)", i, json->cnt-1);
            return FALSE;
        }
#ifdef DUMP
        DBG("index = %d", i);
#endif
        if ( json->rec[i].type == JSON_RECORD || json->rec[i].type == JSON_ARRAY )
        {
            memcpy(json_sub, (JSON*)atol(json->rec[i].value), sizeof(JSON));
            return TRUE;
        }
        else
        {
            return FALSE;   /* types don't match or couldn't convert */
        }
    }

#ifdef DUMP
    DBG("name [%s]", name);
#endif

    for ( i=0; i<json->cnt; ++i )
    {
        if ( 0==strcmp(json->rec[i].name, name) )
        {
//            DBG("lib_json_get_record, found [%s]", name);
            if ( json->rec[i].type == JSON_RECORD || json->rec[i].type == JSON_ARRAY )
            {
                memcpy(json_sub, (JSON*)atol(json->rec[i].value), sizeof(JSON));
                return TRUE;
            }

//            DBG("lib_json_get_record, types of [%s] don't match", name);
            return FALSE;   /* types don't match or couldn't convert */
        }
    }

//    DBG("lib_json_get_record, [%s] not found", name);
    return FALSE;   /* no such field */
}


/* --------------------------------------------------------------------------
   Check system's endianness
-------------------------------------------------------------------------- */
void get_byteorder()
{
    if ( sizeof(long) == 4 )
        get_byteorder32();
    else
        get_byteorder64();
}


/* --------------------------------------------------------------------------
   Check system's endianness
-------------------------------------------------------------------------- */
static void get_byteorder32()
{
    union {
        long l;
        char c[4];
    } test;

    DBG("Checking 32-bit endianness...");

    memset(&test, 0, sizeof(test));

    test.l = 1;

    if ( test.c[3] && !test.c[2] && !test.c[1] && !test.c[0] )
    {
        INF("This is 32-bit Big Endian");
        return;
    }

    if ( !test.c[3] && !test.c[2] && !test.c[1] && test.c[0] )
    {
        INF("This is 32-bit Little Endian");
        return;
    }

    DBG("Unknown Endianness!");
}


/* --------------------------------------------------------------------------
   Check system's endianness
-------------------------------------------------------------------------- */
static void get_byteorder64()
{
    union {
        long l;
        char c[8];
    } test;

    DBG("Checking 64-bit endianness...");

    memset(&test, 0, sizeof(test));

    test.l = 1;

    if ( test.c[7] && !test.c[3] && !test.c[2] && !test.c[1] && !test.c[0] )
    {
        INF("This is 64-bit Big Endian");
        return;
    }

    if ( !test.c[7] && !test.c[3] && !test.c[2] && !test.c[1] && test.c[0] )
    {
        INF("This is 64-bit Little Endian");
        return;
    }

    DBG("Unknown Endianness!");
}


/* --------------------------------------------------------------------------
   Convert database datetime to epoch time
-------------------------------------------------------------------------- */
time_t db2epoch(const char *str)
{

    int     i;
    int     j=0;
    char    part='Y';
    char    strtmp[8];
struct tm   t={0};

/*  DBG("db2epoch: str: [%s]", str); */

    for ( i=0; str[i]; ++i )
    {
        if ( isdigit(str[i]) )
            strtmp[j++] = str[i];
        else    /* end of part */
        {
            strtmp[j] = EOS;
            if ( part == 'Y' )  /* year */
            {
                t.tm_year = atoi(strtmp) - 1900;
                part = 'M';
            }
            else if ( part == 'M' ) /* month */
            {
                t.tm_mon = atoi(strtmp) - 1;
                part = 'D';
            }
            else if ( part == 'D' ) /* day */
            {
                t.tm_mday = atoi(strtmp);
                part = 'H';
            }
            else if ( part == 'H' ) /* hour */
            {
                t.tm_hour = atoi(strtmp);
                part = 'm';
            }
            else if ( part == 'm' ) /* minutes */
            {
                t.tm_min = atoi(strtmp);
                part = 's';
            }
            j = 0;
        }
    }

    /* seconds */

    strtmp[j] = EOS;
    t.tm_sec = atoi(strtmp);

    return mktime(&t);
}


/* --------------------------------------------------------------------------
   Send an email
-------------------------------------------------------------------------- */
bool silgy_email(const char *to, const char *subject, const char *message)
{
    DBG("Sending email to [%s], subject [%s]", to, subject);

#ifndef _WIN32
    char    sender[512];
    char    comm[512];

//#ifndef SILGY_SVC   /* web server mode */

//    sprintf(sender, "%s <noreply@%s>", conn[ci].website, conn[ci].host);

    /* happens when using non-standard port */

//    char    *colon;
//    if ( G_test && (colon=strchr(sender, ':')) )
//    {
//        *colon = '>';
//        *(++colon) = EOS;
//        DBG("sender truncated to [%s]", sender);
//    }
//#else
    sprintf(sender, "%s <%s@%s>", APP_WEBSITE, EMAIL_FROM_USER, APP_DOMAIN);
//#endif

    sprintf(comm, "/usr/lib/sendmail -t -f \"%s\"", sender);

    FILE *mailpipe = popen(comm, "w");

    if ( mailpipe == NULL )
    {
        ERR("Failed to invoke sendmail");
        return FALSE;
    }
    else
    {
        fprintf(mailpipe, "From: %s\n", sender);
        fprintf(mailpipe, "To: %s\n", to);
        fprintf(mailpipe, "Subject: %s\n", subject);
        fprintf(mailpipe, "Content-Type: text/plain; charset=\"utf-8\"\n\n");
        fwrite(message, 1, strlen(message), mailpipe);
        fwrite("\n.\n", 1, 3, mailpipe);
        pclose(mailpipe);
    }

    return TRUE;

#else   /* Windows */

    WAR("There's no email service for Windows");
    return FALSE;

#endif  /* _WIN32 */
}


/* --------------------------------------------------------------------------
   Minify CSS/JS -- new version
   remove all white spaces and new lines unless in quotes
   also remove // style comments
   add a space after some keywords
   return new length
-------------------------------------------------------------------------- */
int silgy_minify(char *dest, const char *src)
{
    char *temp;

    int len = strlen(src);

    if ( !(temp=(char*)malloc(len+1)) )
    {
        ERR("Couldn't allocate %d bytes for silgy_minify", len);
        return 0;
    }

    minify_1(temp, src, len);

    int ret = minify_2(dest, temp);

    free(temp);

    return ret;
}


/* --------------------------------------------------------------------------
   First pass -- only remove comments
-------------------------------------------------------------------------- */
static void minify_1(char *dest, const char *src, int len)
{
    int     i;
    int     j=0;
    bool    opensq=FALSE;       /* single quote */
    bool    opendq=FALSE;       /* double quote */
    bool    openco=FALSE;       /* comment */
    bool    opensc=FALSE;       /* star comment */

    for ( i=0; i<len; ++i )
    {
        if ( !openco && !opensc && !opensq && src[i]=='"' && (i==0 || (i>0 && src[i-1]!='\\')) )
        {
            if ( !opendq )
                opendq = TRUE;
            else
                opendq = FALSE;
        }
        else if ( !openco && !opensc && !opendq && src[i]=='\'' )
        {
            if ( !opensq )
                opensq = TRUE;
            else
                opensq = FALSE;
        }
        else if ( !opensq && !opendq && !openco && !opensc && src[i]=='/' && src[i+1] == '/' )
        {
            openco = TRUE;
        }
        else if ( !opensq && !opendq && !openco && !opensc && src[i]=='/' && src[i+1] == '*' )
        {
            opensc = TRUE;
        }
        else if ( openco && src[i]=='\n' )
        {
            openco = FALSE;
        }
        else if ( opensc && src[i]=='*' && src[i+1]=='/' )
        {
            opensc = FALSE;
            i += 2;
        }

        if ( !openco && !opensc )       /* unless it's a comment ... */
            dest[j++] = src[i];
    }

    dest[j] = EOS;
}


/* --------------------------------------------------------------------------
   Return new length
-------------------------------------------------------------------------- */
static int minify_2(char *dest, const char *src)
{
    int     len;
    int     i;
    int     j=0;
    bool    opensq=FALSE;       /* single quote */
    bool    opendq=FALSE;       /* double quote */
    bool    openbr=FALSE;       /* curly braces */
    bool    openwo=FALSE;       /* word */
    bool    opencc=FALSE;       /* colon */
    bool    skip_ws=FALSE;      /* skip white spaces */
    char    word[256]="";
    int     wi=0;               /* word index */

    len = strlen(src);

    for ( i=0; i<len; ++i )
    {
        if ( !opensq && src[i]=='"' && (i==0 || (i>0 && src[i-1]!='\\')) )
        {
            if ( !opendq )
                opendq = TRUE;
            else
                opendq = FALSE;
        }
        else if ( !opendq && src[i]=='\'' )
        {
            if ( !opensq )
                opensq = TRUE;
            else
                opensq = FALSE;
        }
        else if ( !opensq && !opendq && !openbr && src[i]=='{' )
        {
            openbr = TRUE;
            openwo = FALSE;
            wi = 0;
            skip_ws = TRUE;
        }
        else if ( !opensq && !opendq && openbr && src[i]=='}' )
        {
            openbr = FALSE;
            openwo = FALSE;
            wi = 0;
            skip_ws = TRUE;
        }
        else if ( !opensq && !opendq && openbr && !opencc && src[i]==':' )
        {
            opencc = TRUE;
            openwo = FALSE;
            wi = 0;
            skip_ws = TRUE;
        }
        else if ( !opensq && !opendq && opencc && src[i]==';' )
        {
            opencc = FALSE;
            openwo = FALSE;
            wi = 0;
            skip_ws = TRUE;
        }
        else if ( !opensq && !opendq && !opencc && !openwo && (isalpha(src[i]) || src[i]=='|' || src[i]=='&') ) /* word is starting */
        {
            openwo = TRUE;
        }
        else if ( !opensq && !opendq && openwo && !isalnum(src[i]) && src[i]!='_' && src[i]!='|' && src[i]!='&' )   /* end of word */
        {
            word[wi] = EOS;
            if ( 0==strcmp(word, "var")
                    || 0==strcmp(word, "let")
                    || (0==strcmp(word, "function") && src[i]!='(')
                    || (0==strcmp(word, "else") && src[i]!='{')
                    || 0==strcmp(word, "new")
                    || 0==strcmp(word, "enum")
                    || (0==strcmp(word, "return") && src[i]!=';') )
                dest[j++] = ' ';
            openwo = FALSE;
            wi = 0;
            skip_ws = TRUE;
        }

        if ( opensq || opendq
                || src[i+1] == '|' || src[i+1] == '&'
                || (src[i] != ' ' && src[i] != '\t' && src[i] != '\n' && src[i] != '\r')
                || opencc )
            dest[j++] = src[i];

        if ( openwo )
            word[wi++] = src[i];

        if ( skip_ws )
        {
            while ( src[i+1] && (src[i+1]==' ' || src[i+1]=='\t' || src[i+1]=='\n' || src[i+1]=='\r') ) ++i;
            skip_ws = FALSE;
        }
    }

    dest[j] = EOS;

    return j;
}


/* --------------------------------------------------------------------------
  increment date by 'days' days. Return day of week as well.
  Format: YYYY-MM-DD
-------------------------------------------------------------------------- */
void date_inc(char *str, int days, int *dow)
{
    char    full[32];
    time_t  told, tnew;

    sprintf(full, "%s 00:00:00", str);

    told = db2epoch(full);

    tnew = told + 3600*24*days;

    G_ptm = gmtime(&tnew);
    sprintf(str, "%d-%02d-%02d", G_ptm->tm_year+1900, G_ptm->tm_mon+1, G_ptm->tm_mday);
    *dow = G_ptm->tm_wday;

    G_ptm = gmtime(&G_now);  /* set it back */

}


/* --------------------------------------------------------------------------
  compare the dates
  Format: YYYY-MM-DD
-------------------------------------------------------------------------- */
int date_cmp(const char *str1, const char *str2)
{
    char    full[32];
    time_t  t1, t2;

    sprintf(full, "%s 00:00:00", str1);
    t1 = db2epoch(full);

    sprintf(full, "%s 00:00:00", str2);
    t2 = db2epoch(full);

    return t1 - t2;
}


/* --------------------------------------------------------------------------
   Read the config file
-------------------------------------------------------------------------- */
bool lib_read_conf(const char *file)
{
    FILE    *h_file=NULL;

    /* open the conf file */

    if ( NULL == (h_file=fopen(file, "r")) )
    {
//        printf("Error opening %s, using defaults.\n", file);
        return FALSE;
    }

    /* read content into M_conf for silgy_read_param */

    fseek(h_file, 0, SEEK_END);     /* determine the file size */
    long size = ftell(h_file);
    rewind(h_file);

    if ( (M_conf=(char*)malloc(size+1)) == NULL )
    {
        printf("ERROR: Couldn't get %ld bytes for M_conf\n", size+1);
        fclose(h_file);
        return FALSE;
    }

    fread(M_conf, size, 1, h_file);
    *(M_conf+size) = EOS;

    fclose(h_file);

    return TRUE;
}


#ifdef OLD_CODE
bool lib_read_conf(const char *file)
{
    FILE    *h_file=NULL;
    int     c=0;
    int     i=0;
    char    now_label=1;
    char    now_value=0;
    char    now_comment=0;
    char    label[64]="";
    char    value[256]="";

    /* open the conf file */

    if ( NULL == (h_file=fopen(file, "r")) )
    {
//        printf("Error opening %s, using defaults.\n", file);
        return FALSE;
    }

    /* read content into M_conf for silgy_read_param */

    fseek(h_file, 0, SEEK_END);     /* determine the file size */
    long size = ftell(h_file);
    rewind(h_file);
    if ( (M_conf=(char*)malloc(size+1)) == NULL )
    {
        printf("ERROR: Couldn't get %ld bytes for M_conf\n", size+1);
        fclose(h_file);
        return FALSE;
    }
    fread(M_conf, size, 1, h_file);
    *(M_conf+size) = EOS;
    rewind(h_file);

    /* parse the conf file */

    while ( EOF != (c=fgetc(h_file)) )
    {
        if ( c == '\r' ) continue;

        if ( !now_value && (c == ' ' || c == '\t') ) continue;  /* omit whitespaces */

        if ( c == '\n' )    /* end of value or end of comment or empty line */
        {
            if ( now_value )    /* end of value */
            {
                value[i] = EOS;
#ifndef SILGY_SVC
                eng_set_param(label, value);
//                app_set_param(label, value);
#endif
            }
            now_label = 1;
            now_value = 0;
            now_comment = 0;
            i = 0;
        }
        else if ( now_comment )
        {
            continue;
        }
        else if ( c == '=' )    /* end of label */
        {
            now_label = 0;
            now_value = 1;
            label[i] = EOS;
            i = 0;
        }
        else if ( c == '#' )    /* possible end of value */
        {
            if ( now_value )    /* end of value */
            {
                value[i] = EOS;
#ifndef SILGY_SVC
                eng_set_param(label, value);
//                app_set_param(label, value);
#endif
            }
            now_label = 0;
            now_value = 0;
            now_comment = 1;
            i = 0;
        }
        else if ( now_label )   /* label */
        {
            label[i] = c;
            ++i;
        }
        else if ( now_value )   /* value */
        {
            value[i] = c;
            ++i;
        }
    }

    if ( now_value )    /* end of value */
    {
        value[i] = EOS;
#ifndef SILGY_SVC
        eng_set_param(label, value);
//        app_set_param(label, value);
#endif
    }

    if ( NULL != h_file )
        fclose(h_file);

    return TRUE;
}
#endif  /* OLD_CODE */


/* --------------------------------------------------------------------------
   Get param from config file
---------------------------------------------------------------------------*/
bool silgy_read_param_str(const char *param, char *dest)
{
    char *p;
    int  plen = strlen(param);
#ifdef DUMP
    DBG("silgy_read_param_str [%s]", param);
#endif
    if ( !M_conf )
    {
//        ERR("No config file or not read yet");
        return FALSE;
    }

    if ( (p=strstr(M_conf, param)) == NULL )
    {
//        if ( dest ) dest[0] = EOS;
        return FALSE;
    }

    /* string present but is it label or value? */

    bool found=FALSE;

    while ( p )    /* param may be commented out but present later */
    {
        if ( p > M_conf && *(p-1) != '\n' )  /* commented out or within quotes -- try the next occurence */
        {
#ifdef DUMP
            DBG("param commented out or within quotes, continue search...");
#endif
            p = strstr(++p, param);
        }
        else if ( *(p+plen) != '=' && *(p+plen) != ' ' && *(p+plen) != '\t' )
        {
#ifdef DUMP
            DBG("param does not end with '=', space or tab, continue search...");
#endif
            p = strstr(++p, param);
        }
        else
        {
            found = TRUE;
            break;
        }
    }

    if ( !found )
        return FALSE;

    /* param present ----------------------------------- */

    if ( !dest ) return TRUE;   /* it's only a presence check */


    /* copy value to dest ------------------------------ */

    p += plen;

    while ( *p=='=' || *p==' ' || *p=='\t' )
        ++p;

    int i=0;

    while ( *p != '\r' && *p != '\n' && *p != '#' && *p != EOS )
        dest[i++] = *p++;

    dest[i] = EOS;

    DBG("%s [%s]", param, dest);

    return TRUE;
}


/* --------------------------------------------------------------------------
   Get integer param from config file
---------------------------------------------------------------------------*/
bool silgy_read_param_int(const char *param, int *dest)
{
    char tmp[256];

    if ( silgy_read_param_str(param, tmp) )
    {
        if ( dest ) *dest = atoi(tmp);
        return TRUE;
    }

    return FALSE;
}


/* --------------------------------------------------------------------------
   Create a pid file
-------------------------------------------------------------------------- */
char *lib_create_pid_file(const char *name)
{
static char pidfilename[512];
    FILE    *fpid=NULL;
    char    command[512];

    G_pid = getpid();

#ifdef _WIN32   /* Windows */
    sprintf(pidfilename, "%s\\bin\\%s.pid", G_appdir, name);
#else
    sprintf(pidfilename, "%s/bin/%s.pid", G_appdir, name);
#endif

    /* check if the pid file already exists */

    if ( access(pidfilename, F_OK) != -1 )
    {
        WAR("PID file already exists");
        INF("Killing the old process...");
#ifdef _WIN32   /* Windows */
        /* open the pid file and read process id */
        if ( NULL == (fpid=fopen(pidfilename, "r")) )
        {
            ERR("Couldn't open pid file for reading");
            return NULL;
        }
        fseek(fpid, 0, SEEK_END);     /* determine the file size */
        int fsize = ftell(fpid);
        if ( fsize < 1 || fsize > 60 )
        {
            fclose(fpid);
            ERR("Something's wrong with the pid file size (%d bytes)", fsize);
            return NULL;
        }
        rewind(fpid);
        char oldpid[64];
        fread(oldpid, fsize, 1, fpid);
        fclose(fpid);
        oldpid[fsize] = EOS;
        DBG("oldpid [%s]", oldpid);

        msleep(100);

        sprintf(command, "taskkill /pid %s", oldpid);
#else
        sprintf(command, "kill `cat %s`", pidfilename);
#endif  /* _WIN32 */
//        system(command);

//        msleep(100);

        INF("Removing pid file...");
#ifdef _WIN32   /* Windows */
        sprintf(command, "del %s", pidfilename);
#else
        sprintf(command, "rm %s", pidfilename);
#endif
        system(command);

        msleep(100);
    }

    /* create a pid file */

    if ( NULL == (fpid=fopen(pidfilename, "w")) )
    {
        INF("Tried to create [%s]", pidfilename);
        ERR("Failed to create pid file, errno = %d (%s)", errno, strerror(errno));
        return NULL;
    }

    /* write pid to pid file */

    if ( fprintf(fpid, "%d", G_pid) < 1 )
    {
        ERR("Couldn't write to pid file, errno = %d (%s)", errno, strerror(errno));
        return NULL;
    }

    fclose(fpid);

    return pidfilename;
}


/* --------------------------------------------------------------------------
   Attach to shared memory segment
-------------------------------------------------------------------------- */
bool lib_shm_create(long bytes)
{
#ifndef _WIN32
    key_t key;

    /* Create unique key via call to ftok() */
    key = ftok(".", 'S');

    /* Open the shared memory segment - create if necessary */
    if ( (M_shmid=shmget(key, bytes, IPC_CREAT|IPC_EXCL|0666)) == -1 )
    {
        printf("Shared memory segment exists - opening as client\n");

        /* Segment probably already exists - try as a client */
        if ( (M_shmid=shmget(key, bytes, 0)) == -1 )
        {
            perror("shmget");
            return FALSE;
        }
    }
    else
    {
        printf("Creating new shared memory segment\n");
    }

    /* Attach (map) the shared memory segment into the current process */
    if ( (G_shm_segptr=(char*)shmat(M_shmid, 0, 0)) == (char*)-1 )
    {
        perror("shmat");
        return FALSE;
    }
#endif
    return TRUE;
}


/* --------------------------------------------------------------------------
   Delete shared memory segment
-------------------------------------------------------------------------- */
void lib_shm_delete(long bytes)
{
#ifndef _WIN32
    if ( lib_shm_create(bytes) )
    {
        shmctl(M_shmid, IPC_RMID, 0);
        printf("Shared memory segment marked for deletion\n");
    }
#endif
}


/* --------------------------------------------------------------------------
   Start a log
-------------------------------------------------------------------------- */
bool log_start(const char *prefix, bool test)
{
    char    fprefix[64]="";     /* formatted prefix */
    char    fname[512];         /* file name */
    char    ffname[512];        /* full file name */

    if ( G_logLevel < 1 ) return TRUE;  /* no log */

    if ( G_logToStdout != 1 )   /* log to a file */
    {
        if ( M_log_fd != NULL && M_log_fd != stdout ) return TRUE;  /* already started */

        if ( prefix && prefix[0] )
            sprintf(fprefix, "%s_", prefix);

        sprintf(fname, "%s%d%02d%02d_%02d%02d", fprefix, G_ptm->tm_year+1900, G_ptm->tm_mon+1, G_ptm->tm_mday, G_ptm->tm_hour, G_ptm->tm_min);

        if ( test )
            sprintf(ffname, "%s_t.log", fname);
        else
            sprintf(ffname, "%s.log", fname);

        /* first try in SILGYDIR --------------------------------------------- */

        if ( G_appdir[0] )
        {
            char fffname[512];       /* full file name with path */
            sprintf(fffname, "%s/logs/%s", G_appdir, ffname);
            if ( NULL == (M_log_fd=fopen(fffname, "a")) )
            {
                if ( NULL == (M_log_fd=fopen(ffname, "a")) )  /* try current dir */
                {
                    printf("ERROR: Couldn't open log file.\n");
                    return FALSE;
                }
            }
        }
        else    /* no SILGYDIR -- try current dir */
        {
            if ( NULL == (M_log_fd=fopen(ffname, "a")) )
            {
                printf("ERROR: Couldn't open log file.\n");
                return FALSE;
            }
        }
    }

    fprintf(M_log_fd, LOG_LINE_LONG_N);

    ALWAYS(" %s  Starting %s's log. Server version: %s, app version: %s", G_dt, APP_WEBSITE, WEB_SERVER_VERSION, APP_VERSION);

    fprintf(M_log_fd, LOG_LINE_LONG_NN);

    return TRUE;
}


/* --------------------------------------------------------------------------
   Write to log with date/time
-------------------------------------------------------------------------- */
void log_write_time(int level, const char *message, ...)
{
    if ( level > G_logLevel ) return;

    /* output timestamp */

    fprintf(M_log_fd, "[%s] ", G_dt+11);

    if ( LOG_ERR == level )
        fprintf(M_log_fd, "ERROR: ");
    else if ( LOG_WAR == level )
        fprintf(M_log_fd, "WARNING: ");

    /* compile message with arguments into buffer */

    va_list plist;
    char buffer[MAX_LOG_STR_LEN+1+64];

    va_start(plist, message);
    vsprintf(buffer, message, plist);
    va_end(plist);

    /* write to the log file */

    fprintf(M_log_fd, "%s\n", buffer);

#ifdef DUMP
    fflush(M_log_fd);
#else
    if ( G_logLevel >= LOG_DBG || level == LOG_ERR ) fflush(M_log_fd);
#endif
}


/* --------------------------------------------------------------------------
   Write to log
-------------------------------------------------------------------------- */
void log_write(int level, const char *message, ...)
{
    if ( level > G_logLevel ) return;

    if ( LOG_ERR == level )
        fprintf(M_log_fd, "ERROR: ");
    else if ( LOG_WAR == level )
        fprintf(M_log_fd, "WARNING: ");

    /* compile message with arguments into buffer */

    va_list plist;
    char buffer[MAX_LOG_STR_LEN+1+64];

    va_start(plist, message);
    vsprintf(buffer, message, plist);
    va_end(plist);

    /* write to the log file */

    fprintf(M_log_fd, "%s\n", buffer);

#ifdef DUMP
    fflush(M_log_fd);
#else
    if ( G_logLevel >= LOG_DBG || level == LOG_ERR ) fflush(M_log_fd);
#endif
}


/* --------------------------------------------------------------------------
   Write looong message to a log or --
   its first (MAX_LOG_STR_LEN-50) part if it's longer
-------------------------------------------------------------------------- */
void log_long(const char *message, long len, const char *desc)
{
    if ( G_logLevel < LOG_DBG ) return;

    char buffer[MAX_LOG_STR_LEN+1];

    if ( len < MAX_LOG_STR_LEN-50 )
    {
        strncpy(buffer, message, len);
        buffer[len] = EOS;
    }
    else
    {
        strncpy(buffer, message, MAX_LOG_STR_LEN-50);
        strcpy(buffer+MAX_LOG_STR_LEN-50, " (...)");
    }

    DBG("%s:\n\n[%s]\n", desc, buffer);
}


/* --------------------------------------------------------------------------
   Flush log
-------------------------------------------------------------------------- */
void log_flush()
{
    if ( M_log_fd != NULL )
        fflush(M_log_fd);
}


/* --------------------------------------------------------------------------
   Close log
-------------------------------------------------------------------------- */
void log_finish()
{
    if ( G_logLevel > 0 )
        ALWAYS("Closing log");

    if ( M_log_fd != NULL && M_log_fd != stdout )
    {
        fclose(M_log_fd);
        M_log_fd = stdout;
    }
}


#ifdef ICONV
/* --------------------------------------------------------------------------
   Convert string
-------------------------------------------------------------------------- */
char *silgy_convert(char *src, const char *cp_from, const char *cp_to)
{
static char dst[4096];

    iconv_t cd = iconv_open(cp_to, cp_from);

    if ( cd == (iconv_t)-1 )
    {
        strcpy(dst, "iconv_open failed");
        return dst;
    }

    char *in_buf = src;
    size_t in_left = strlen(src);

    char *out_buf = &dst[0];
    size_t out_left = 4095;

    do
    {
        if ( iconv(cd, &in_buf, &in_left, &out_buf, &out_left) == (size_t)-1 )
        {
            strcpy(dst, "iconv failed");
            return dst;
        }
    } while (in_left > 0 && out_left > 0);

    *out_buf = 0;

    iconv_close(cd);

    return dst;
}
#endif  /* ICONV */








/* ================================================================================================ */
/* MD5                                                                                              */
/* ================================================================================================ */
/*
 * This is an OpenSSL-compatible implementation of the RSA Data Security, Inc.
 * MD5 Message-Digest Algorithm (RFC 1321).
 *
 * Homepage:
 * http://openwall.info/wiki/people/solar/software/public-domain-source-code/md5
 *
 * Author:
 * Alexander Peslyak, better known as Solar Designer <solar at openwall.com>
 *
 * This software was written by Alexander Peslyak in 2001.  No copyright is
 * claimed, and the software is hereby placed in the public domain.
 * In case this attempt to disclaim copyright and place the software in the
 * public domain is deemed null and void, then the software is
 * Copyright (c) 2001 Alexander Peslyak and it is hereby released to the
 * general public under the following terms:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * There's ABSOLUTELY NO WARRANTY, express or implied.
 *
 */
 
#ifdef HAVE_OPENSSL
#include <openssl/md5.h>
#elif !defined(_MD5_H)
#define _MD5_H
 
/* Any 32-bit or wider unsigned integer data type will do */
typedef unsigned int MD5_u32plus;
 
typedef struct {
	MD5_u32plus lo, hi;
	MD5_u32plus a, b, c, d;
	unsigned char buffer[64];
	MD5_u32plus block[16];
} MD5_CTX;
 
extern void MD5_Init(MD5_CTX *ctx);
extern void MD5_Update(MD5_CTX *ctx, const void *data, unsigned long size);
extern void MD5_Final(unsigned char *result, MD5_CTX *ctx);
 
#endif



#ifndef HAVE_OPENSSL
 
#include <string.h>
 
/*
 * The basic MD5 functions.
 *
 * F and G are optimized compared to their RFC 1321 definitions for
 * architectures that lack an AND-NOT instruction, just like in Colin Plumb's
 * implementation.
 */
#define F(x, y, z)			((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z)			((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z)			(((x) ^ (y)) ^ (z))
#define H2(x, y, z)			((x) ^ ((y) ^ (z)))
#define I(x, y, z)			((y) ^ ((x) | ~(z)))
 
/*
 * The MD5 transformation for all four rounds.
 */
#define STEP(f, a, b, c, d, x, t, s) \
	(a) += f((b), (c), (d)) + (x) + (t); \
	(a) = (((a) << (s)) | (((a) & 0xffffffff) >> (32 - (s)))); \
	(a) += (b);
 
/*
 * SET reads 4 input bytes in little-endian byte order and stores them in a
 * properly aligned word in host byte order.
 *
 * The check for little-endian architectures that tolerate unaligned memory
 * accesses is just an optimization.  Nothing will break if it fails to detect
 * a suitable architecture.
 *
 * Unfortunately, this optimization may be a C strict aliasing rules violation
 * if the caller's data buffer has effective type that cannot be aliased by
 * MD5_u32plus.  In practice, this problem may occur if these MD5 routines are
 * inlined into a calling function, or with future and dangerously advanced
 * link-time optimizations.  For the time being, keeping these MD5 routines in
 * their own translation unit avoids the problem.
 */
#if defined(__i386__) || defined(__x86_64__) || defined(__vax__)
#define SET(n) \
	(*(MD5_u32plus *)&ptr[(n) * 4])
#define GET(n) \
	SET(n)
#else
#define SET(n) \
	(ctx->block[(n)] = \
	(MD5_u32plus)ptr[(n) * 4] | \
	((MD5_u32plus)ptr[(n) * 4 + 1] << 8) | \
	((MD5_u32plus)ptr[(n) * 4 + 2] << 16) | \
	((MD5_u32plus)ptr[(n) * 4 + 3] << 24))
#define GET(n) \
	(ctx->block[(n)])
#endif
 
/*
 * This processes one or more 64-byte data blocks, but does NOT update the bit
 * counters.  There are no alignment requirements.
 */
static const void *body(MD5_CTX *ctx, const void *data, unsigned long size)
{
	const unsigned char *ptr;
	MD5_u32plus a, b, c, d;
	MD5_u32plus saved_a, saved_b, saved_c, saved_d;
 
	ptr = (const unsigned char *)data;
 
	a = ctx->a;
	b = ctx->b;
	c = ctx->c;
	d = ctx->d;
 
	do {
		saved_a = a;
		saved_b = b;
		saved_c = c;
		saved_d = d;
 
/* Round 1 */
		STEP(F, a, b, c, d, SET(0), 0xd76aa478, 7)
		STEP(F, d, a, b, c, SET(1), 0xe8c7b756, 12)
		STEP(F, c, d, a, b, SET(2), 0x242070db, 17)
		STEP(F, b, c, d, a, SET(3), 0xc1bdceee, 22)
		STEP(F, a, b, c, d, SET(4), 0xf57c0faf, 7)
		STEP(F, d, a, b, c, SET(5), 0x4787c62a, 12)
		STEP(F, c, d, a, b, SET(6), 0xa8304613, 17)
		STEP(F, b, c, d, a, SET(7), 0xfd469501, 22)
		STEP(F, a, b, c, d, SET(8), 0x698098d8, 7)
		STEP(F, d, a, b, c, SET(9), 0x8b44f7af, 12)
		STEP(F, c, d, a, b, SET(10), 0xffff5bb1, 17)
		STEP(F, b, c, d, a, SET(11), 0x895cd7be, 22)
		STEP(F, a, b, c, d, SET(12), 0x6b901122, 7)
		STEP(F, d, a, b, c, SET(13), 0xfd987193, 12)
		STEP(F, c, d, a, b, SET(14), 0xa679438e, 17)
		STEP(F, b, c, d, a, SET(15), 0x49b40821, 22)
 
/* Round 2 */
		STEP(G, a, b, c, d, GET(1), 0xf61e2562, 5)
		STEP(G, d, a, b, c, GET(6), 0xc040b340, 9)
		STEP(G, c, d, a, b, GET(11), 0x265e5a51, 14)
		STEP(G, b, c, d, a, GET(0), 0xe9b6c7aa, 20)
		STEP(G, a, b, c, d, GET(5), 0xd62f105d, 5)
		STEP(G, d, a, b, c, GET(10), 0x02441453, 9)
		STEP(G, c, d, a, b, GET(15), 0xd8a1e681, 14)
		STEP(G, b, c, d, a, GET(4), 0xe7d3fbc8, 20)
		STEP(G, a, b, c, d, GET(9), 0x21e1cde6, 5)
		STEP(G, d, a, b, c, GET(14), 0xc33707d6, 9)
		STEP(G, c, d, a, b, GET(3), 0xf4d50d87, 14)
		STEP(G, b, c, d, a, GET(8), 0x455a14ed, 20)
		STEP(G, a, b, c, d, GET(13), 0xa9e3e905, 5)
		STEP(G, d, a, b, c, GET(2), 0xfcefa3f8, 9)
		STEP(G, c, d, a, b, GET(7), 0x676f02d9, 14)
		STEP(G, b, c, d, a, GET(12), 0x8d2a4c8a, 20)
 
/* Round 3 */
		STEP(H, a, b, c, d, GET(5), 0xfffa3942, 4)
		STEP(H2, d, a, b, c, GET(8), 0x8771f681, 11)
		STEP(H, c, d, a, b, GET(11), 0x6d9d6122, 16)
		STEP(H2, b, c, d, a, GET(14), 0xfde5380c, 23)
		STEP(H, a, b, c, d, GET(1), 0xa4beea44, 4)
		STEP(H2, d, a, b, c, GET(4), 0x4bdecfa9, 11)
		STEP(H, c, d, a, b, GET(7), 0xf6bb4b60, 16)
		STEP(H2, b, c, d, a, GET(10), 0xbebfbc70, 23)
		STEP(H, a, b, c, d, GET(13), 0x289b7ec6, 4)
		STEP(H2, d, a, b, c, GET(0), 0xeaa127fa, 11)
		STEP(H, c, d, a, b, GET(3), 0xd4ef3085, 16)
		STEP(H2, b, c, d, a, GET(6), 0x04881d05, 23)
		STEP(H, a, b, c, d, GET(9), 0xd9d4d039, 4)
		STEP(H2, d, a, b, c, GET(12), 0xe6db99e5, 11)
		STEP(H, c, d, a, b, GET(15), 0x1fa27cf8, 16)
		STEP(H2, b, c, d, a, GET(2), 0xc4ac5665, 23)
 
/* Round 4 */
		STEP(I, a, b, c, d, GET(0), 0xf4292244, 6)
		STEP(I, d, a, b, c, GET(7), 0x432aff97, 10)
		STEP(I, c, d, a, b, GET(14), 0xab9423a7, 15)
		STEP(I, b, c, d, a, GET(5), 0xfc93a039, 21)
		STEP(I, a, b, c, d, GET(12), 0x655b59c3, 6)
		STEP(I, d, a, b, c, GET(3), 0x8f0ccc92, 10)
		STEP(I, c, d, a, b, GET(10), 0xffeff47d, 15)
		STEP(I, b, c, d, a, GET(1), 0x85845dd1, 21)
		STEP(I, a, b, c, d, GET(8), 0x6fa87e4f, 6)
		STEP(I, d, a, b, c, GET(15), 0xfe2ce6e0, 10)
		STEP(I, c, d, a, b, GET(6), 0xa3014314, 15)
		STEP(I, b, c, d, a, GET(13), 0x4e0811a1, 21)
		STEP(I, a, b, c, d, GET(4), 0xf7537e82, 6)
		STEP(I, d, a, b, c, GET(11), 0xbd3af235, 10)
		STEP(I, c, d, a, b, GET(2), 0x2ad7d2bb, 15)
		STEP(I, b, c, d, a, GET(9), 0xeb86d391, 21)
 
		a += saved_a;
		b += saved_b;
		c += saved_c;
		d += saved_d;
 
		ptr += 64;
	} while (size -= 64);
 
	ctx->a = a;
	ctx->b = b;
	ctx->c = c;
	ctx->d = d;
 
	return ptr;
}
 
void MD5_Init(MD5_CTX *ctx)
{
	ctx->a = 0x67452301;
	ctx->b = 0xefcdab89;
	ctx->c = 0x98badcfe;
	ctx->d = 0x10325476;
 
	ctx->lo = 0;
	ctx->hi = 0;
}
 
void MD5_Update(MD5_CTX *ctx, const void *data, unsigned long size)
{
	MD5_u32plus saved_lo;
	unsigned long used, available;
 
	saved_lo = ctx->lo;
	if ((ctx->lo = (saved_lo + size) & 0x1fffffff) < saved_lo)
		ctx->hi++;
	ctx->hi += size >> 29;
 
	used = saved_lo & 0x3f;
 
	if (used) {
		available = 64 - used;
 
		if (size < available) {
			memcpy(&ctx->buffer[used], data, size);
			return;
		}
 
		memcpy(&ctx->buffer[used], data, available);
		data = (const unsigned char *)data + available;
		size -= available;
		body(ctx, ctx->buffer, 64);
	}
 
	if (size >= 64) {
		data = body(ctx, data, size & ~(unsigned long)0x3f);
		size &= 0x3f;
	}
 
	memcpy(ctx->buffer, data, size);
}
 
#define MD5_OUT(dst, src) \
	(dst)[0] = (unsigned char)(src); \
	(dst)[1] = (unsigned char)((src) >> 8); \
	(dst)[2] = (unsigned char)((src) >> 16); \
	(dst)[3] = (unsigned char)((src) >> 24);
 
void MD5_Final(unsigned char *result, MD5_CTX *ctx)
{
	unsigned long used, available;
 
	used = ctx->lo & 0x3f;
 
	ctx->buffer[used++] = 0x80;
 
	available = 64 - used;
 
	if (available < 8) {
		memset(&ctx->buffer[used], 0, available);
		body(ctx, ctx->buffer, 64);
		used = 0;
		available = 64;
	}
 
	memset(&ctx->buffer[used], 0, available - 8);
 
	ctx->lo <<= 3;
	MD5_OUT(&ctx->buffer[56], ctx->lo)
	MD5_OUT(&ctx->buffer[60], ctx->hi)
 
	body(ctx, ctx->buffer, 64);
 
	MD5_OUT(&result[0], ctx->a)
	MD5_OUT(&result[4], ctx->b)
	MD5_OUT(&result[8], ctx->c)
	MD5_OUT(&result[12], ctx->d)
 
	memset(ctx, 0, sizeof(*ctx));
}
 
#endif


/* --------------------------------------------------------------------------
   Return MD5 hash in the form of hex string
-------------------------------------------------------------------------- */
char *md5(const char* str)
{
static char result[33];
    unsigned char digest[16];

    MD5_CTX context;

    MD5_Init(&context);
    MD5_Update(&context, str, strlen(str));
    MD5_Final(digest, &context);

    int i;
    for ( i=0; i<16; ++i )
        sprintf(&result[i*2], "%02x", (unsigned int)digest[i]);

    return result;
}




/* ================================================================================================ */
/* Base64                                                                                           */
/* ================================================================================================ */
/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/* ====================================================================
 * Copyright (c) 1995-1999 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache"
 *    nor may "Apache" appear in their names without prior written
 *    permission of the Apache Group.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */

/* Base64 encoder/decoder. Originally Apache file ap_base64.c
 */

/* aaaack but it's fast and const should make it shared text page. */
static const unsigned char pr2six[256] =
{
    /* ASCII table */
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
    64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

int Base64decode_len(const char *bufcoded)
{
    int nbytesdecoded;
    register const unsigned char *bufin;
    register int nprbytes;

    bufin = (const unsigned char *) bufcoded;
    while (pr2six[*(bufin++)] <= 63);

    nprbytes = (bufin - (const unsigned char *) bufcoded) - 1;
    nbytesdecoded = ((nprbytes + 3) / 4) * 3;

    return nbytesdecoded + 1;
}

int Base64decode(char *bufplain, const char *bufcoded)
{
    int nbytesdecoded;
    register const unsigned char *bufin;
    register unsigned char *bufout;
    register int nprbytes;

    bufin = (const unsigned char *) bufcoded;
    while (pr2six[*(bufin++)] <= 63);
    nprbytes = (bufin - (const unsigned char *) bufcoded) - 1;
    nbytesdecoded = ((nprbytes + 3) / 4) * 3;

    bufout = (unsigned char *) bufplain;
    bufin = (const unsigned char *) bufcoded;

    while (nprbytes > 4) {
    *(bufout++) =
        (unsigned char) (pr2six[*bufin] << 2 | pr2six[bufin[1]] >> 4);
    *(bufout++) =
        (unsigned char) (pr2six[bufin[1]] << 4 | pr2six[bufin[2]] >> 2);
    *(bufout++) =
        (unsigned char) (pr2six[bufin[2]] << 6 | pr2six[bufin[3]]);
    bufin += 4;
    nprbytes -= 4;
    }

    /* Note: (nprbytes == 1) would be an error, so just ingore that case */
    if (nprbytes > 1) {
    *(bufout++) =
        (unsigned char) (pr2six[*bufin] << 2 | pr2six[bufin[1]] >> 4);
    }
    if (nprbytes > 2) {
    *(bufout++) =
        (unsigned char) (pr2six[bufin[1]] << 4 | pr2six[bufin[2]] >> 2);
    }
    if (nprbytes > 3) {
    *(bufout++) =
        (unsigned char) (pr2six[bufin[2]] << 6 | pr2six[bufin[3]]);
    }

    *(bufout++) = '\0';
    nbytesdecoded -= (4 - nprbytes) & 3;
    return nbytesdecoded;
}

static const char basis_64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64encode_len(int len)
{
    return ((len + 2) / 3 * 4) + 1;
}

int Base64encode(char *encoded, const char *string, int len)
{
    int i;
    char *p;

    p = encoded;
    for (i = 0; i < len - 2; i += 3) {
    *p++ = basis_64[(string[i] >> 2) & 0x3F];
    *p++ = basis_64[((string[i] & 0x3) << 4) |
                    ((int) (string[i + 1] & 0xF0) >> 4)];
    *p++ = basis_64[((string[i + 1] & 0xF) << 2) |
                    ((int) (string[i + 2] & 0xC0) >> 6)];
    *p++ = basis_64[string[i + 2] & 0x3F];
    }
    if (i < len) {
    *p++ = basis_64[(string[i] >> 2) & 0x3F];
    if (i == (len - 1)) {
        *p++ = basis_64[((string[i] & 0x3) << 4)];
        *p++ = '=';
    }
    else {
        *p++ = basis_64[((string[i] & 0x3) << 4) |
                        ((int) (string[i + 1] & 0xF0) >> 4)];
        *p++ = basis_64[((string[i + 1] & 0xF) << 2)];
    }
    *p++ = '=';
    }

    *p++ = '\0';
    return p - encoded;
}



/* ================================================================================================ */
/* SHA1                                                                                             */
/* ================================================================================================ */
/*
SHA-1 in C
By Steve Reid <sreid@sea-to-sky.net>
100% Public Domain

-----------------
Modified 7/98
By James H. Brown <jbrown@burgoyne.com>
Still 100% Public Domain

Corrected a problem which generated improper hash values on 16 bit machines
Routine SHA1Update changed from
    void SHA1Update(SHA1_CTX* context, unsigned char* data, unsigned int
len)
to
    void SHA1Update(SHA1_CTX* context, unsigned char* data, unsigned
long len)

The 'len' parameter was declared an int which works fine on 32 bit machines.
However, on 16 bit machines an int is too small for the shifts being done
against
it.  This caused the hash function to generate incorrect values if len was
greater than 8191 (8K - 1) due to the 'len << 3' on line 3 of SHA1Update().

Since the file IO in main() reads 16K at a time, any file 8K or larger would
be guaranteed to generate the wrong hash (e.g. Test Vector #3, a million
"a"s).

I also changed the declaration of variables i & j in SHA1Update to
unsigned long from unsigned int for the same reason.

These changes should make no difference to any 32 bit implementations since
an
int and a long are the same size in those environments.

--
I also corrected a few compiler warnings generated by Borland C.
1. Added #include <process.h> for exit() prototype
2. Removed unused variable 'j' in SHA1Final
3. Changed exit(0) to return(0) at end of main.

ALL changes I made can be located by searching for comments containing 'JHB'
-----------------
Modified 8/98
By Steve Reid <sreid@sea-to-sky.net>
Still 100% public domain

1- Removed #include <process.h> and used return() instead of exit()
2- Fixed overwriting of finalcount in SHA1Final() (discovered by Chris Hall)
3- Changed email address from steve@edmweb.com to sreid@sea-to-sky.net

-----------------
Modified 4/01
By Saul Kravitz <Saul.Kravitz@celera.com>
Still 100% PD
Modified to run on Compaq Alpha hardware.

-----------------
Modified 07/2002
By Ralph Giles <giles@ghostscript.com>
Still 100% public domain
modified for use with stdint types, autoconf
code cleanup, removed attribution comments
switched SHA1Final() argument order for consistency
use SHA1_ prefix for public api
move public api to sha1.h
*/

/*
Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/

/*#define WORDS_BIGENDIAN        on AIX only! */

#define SHA1HANDSOFF

static void SHA1_Transform2(uint32_t state[5], const uint8_t buffer[64]);

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
/* FIXME: can we do this in an endian-proof way? */
#ifdef WORDS_BIGENDIAN
#define blk0(i) block->l[i]
#else
#define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
    |(rol(block->l[i],8)&0x00FF00FF))
#endif
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
    ^block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in libSHA1 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);


static void libSHA1_Init(SHA1_CTX* context);
static void libSHA1_Update(SHA1_CTX* context, const uint8_t* data, const size_t len);
static void libSHA1_Final(SHA1_CTX* context, uint8_t digest[SHA1_DIGEST_SIZE]);



/* Hash a single 512-bit block. This is the core of the algorithm. */
static void SHA1_Transform2(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t a, b, c, d, e;
    typedef union {
        uint8_t c[64];
        uint32_t l[16];
    } CHAR64LONG16;
    CHAR64LONG16* block;

#ifdef SHA1HANDSOFF
    static uint8_t workspace[64];
    block = (CHAR64LONG16*)workspace;
    memcpy(block, buffer, 64);
#else
    block = (CHAR64LONG16*)buffer;
#endif

    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);

    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;

    /* Wipe variables */
    a = b = c = d = e = 0;
}


/* SHA1Init - Initialize new context */
static void libSHA1_Init(SHA1_CTX* context)
{
    /* libSHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}


/* Run your data through this. */
static void libSHA1_Update(SHA1_CTX* context, const uint8_t* data, const size_t len)
{
    size_t i, j;

    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += len << 3) < (len << 3)) context->count[1]++;
    context->count[1] += (len >> 29);
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64-j));
        SHA1_Transform2(context->state, context->buffer);
        for ( ; i + 63 < len; i += 64) {
            SHA1_Transform2(context->state, data + i);
        }
        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}


/* Add padding and return the message digest. */
static void libSHA1_Final(SHA1_CTX* context, uint8_t digest[SHA1_DIGEST_SIZE])
{
    uint32_t i;
    uint8_t  finalcount[8];

    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)]
         >> ((3-(i & 3)) * 8) ) & 255);  /* Endian independent */
    }
    libSHA1_Update(context, (uint8_t *)"\200", 1);
    while ((context->count[0] & 504) != 448) {
        libSHA1_Update(context, (uint8_t *)"\0", 1);
    }
    libSHA1_Update(context, finalcount, 8);  /* Should cause a SHA1_Transform() */
    for (i = 0; i < SHA1_DIGEST_SIZE; i++) {
        digest[i] = (uint8_t)
         ((context->state[i>>2] >> ((3-(i & 3)) * 8) ) & 255);
    }

    /* Wipe variables */
    i = 0;
    memset(context->buffer, 0, 64);
    memset(context->state, 0, 20);
    memset(context->count, 0, 8);
    memset(finalcount, 0, 8);   /* SWR */

#ifdef SHA1HANDSOFF  /* make SHA1Transform overwrite its own static vars */
    SHA1_Transform2(context->state, context->buffer);
#endif
}


void libSHA1(unsigned char *ptr, unsigned int size, unsigned char *outbuf)
{
  SHA1_CTX ctx;

  libSHA1_Init(&ctx);
  libSHA1_Update(&ctx, ptr, size);
  libSHA1_Final(&ctx, outbuf);
}



void digest_to_hex(const uint8_t digest[SHA1_DIGEST_SIZE], char *output)
{
    int i,j;
    char *c = output;

    for (i = 0; i < SHA1_DIGEST_SIZE/4; i++) {
        for (j = 0; j < 4; j++) {
            sprintf(c,"%02X", digest[i*4+j]);
            c += 2;
        }
        sprintf(c, " ");
        c += 1;
    }
    *(c - 1) = '\0';
}




#ifdef _WIN32   /* Windows */
/* --------------------------------------------------------------------------
   Windows port of getpid
-------------------------------------------------------------------------- */
int getpid()
{
    return GetCurrentProcessId();
}


/* --------------------------------------------------------------------------
   Windows port of clock_gettime
   https://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows
-------------------------------------------------------------------------- */
#define exp7           10000000     // 1E+7
#define exp9         1000000000     // 1E+9
#define w2ux 116444736000000000     // 1 Jan 1601 to 1 Jan 1970

static void unix_time(struct timespec *spec)
{
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -= w2ux;
    spec->tv_sec = wintime / exp7;
    spec->tv_nsec = wintime % exp7 * 100;
}

int clock_gettime_win(struct timespec *spec)
{
   static  struct timespec startspec;
   static double ticks2nano;
   static __int64 startticks, tps=0;
   __int64 tmp, curticks;

   QueryPerformanceFrequency((LARGE_INTEGER*)&tmp); // some strange system can possibly change freq?

   if ( tps != tmp )
   {
       tps = tmp; // init ONCE
       QueryPerformanceCounter((LARGE_INTEGER*)&startticks);
       unix_time(&startspec);
       ticks2nano = (double)exp9 / tps;
   }

   QueryPerformanceCounter((LARGE_INTEGER*)&curticks);
   curticks -= startticks;
   spec->tv_sec = startspec.tv_sec + (curticks / tps);
   spec->tv_nsec = startspec.tv_nsec + (double)(curticks % tps) * ticks2nano;
   if ( !(spec->tv_nsec < exp9) )
   {
       ++spec->tv_sec;
       spec->tv_nsec -= exp9;
   }

   return 0;
}


/* --------------------------------------------------------------------------
   Windows port of stpcpy
-------------------------------------------------------------------------- */
char *stpcpy(char *dest, const char *src)
{
    register char *d=dest;
    register const char *s=src;

    do
        *d++ = *s;
    while (*s++ != '\0');

    return d - 1;
}


/* --------------------------------------------------------------------------
   Windows port of stpncpy
-------------------------------------------------------------------------- */
char *stpncpy(char *dest, const char *src, unsigned int len)
{
    register char *d=dest;
    register const char *s=src;
    int count=0;

    do
        *d++ = *s;
    while (*s++ != '\0' && ++count<len);

    return d - 1;
}
#endif  /* _WIN32 */


#ifndef strnstr
/* --------------------------------------------------------------------------
   Windows port of strnstr
-------------------------------------------------------------------------- */
char *strnstr(const char *haystack, const char *needle, size_t len)
{
    int i;
    size_t needle_len;

    if ( 0 == (needle_len = strnlen(needle, len)) )
        return (char*)haystack;

    for ( i=0; i<=(int)(len-needle_len); ++i )
    {
        if ( (haystack[0] == needle[0]) && (0 == strncmp(haystack, needle, needle_len)) )
            return (char*)haystack;

        ++haystack;
    }

    return NULL;
}
#endif
