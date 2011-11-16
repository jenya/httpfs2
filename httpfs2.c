/*
 * HTTPFS: import a file from a web server to local file system
 * the main use is, to mount an iso on a web server with loop device
 *
 * depends on:
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 *
 * This program can be distributed under the terms of the GNU GPL.
 *
 */

/*
 * (c) 2006  hmb  marionraven at users.sourceforge.net
 *
 */

/*
 * Modified to work with fuse 2.7.
 * Added keepalive
 * The passthru functionality removed to simplify the code.
 * (c) 2008-2011 Michal Suchanek <hramrach@centrum.cz>
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <stddef.h>
#include <inttypes.h>

#ifdef USE_THREAD
#include <pthread.h>
static pthread_key_t url_key;
#define FUSE_LOOP fuse_session_loop_mt
#else
#define FUSE_LOOP fuse_session_loop
#endif

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define TIMEOUT 30
#define CONSOLE "/dev/console"
#define HEADER_SIZE 1024
#define VERSION "0.1.4 \"Monolith\""

static char* argv0;

#define MAX_REQUEST (32*1024)
#define SOCK_CLOSED 0
#define SOCK_OPEN 1
#define SOCK_KEEPALIVE 2

typedef struct url {
    int proto;
    long timeout;
    char * host; /*hostname*/
    int port;
    char * path; /*get path*/
    char * name; /*file name*/
#ifdef USE_AUTH
    char * auth; /*encoded auth data*/
#endif
#ifdef RETRY_ON_RESET
    int retry_reset; /*retry reset connections*/
#endif
    int sockfd;
    int sock_type;
#ifdef USE_SSL
    SSL_CTX* ssl_ctx;
    SSL* ssl;
#endif
    char * req_buf;
    size_t req_buf_size;
    off_t file_size;
    time_t last_modified;
} struct_url;

static struct_url main_url;

static off_t get_stat(struct_url*, struct stat * stbuf);
static ssize_t get_data(struct_url*, off_t start, size_t size);
static int open_client_socket(struct_url *url);
static int close_client_socket(struct_url *url);
static int close_client_force(struct_url *url);
static struct_url * thread_setup(void);
static void destroy_url_copy(void *);

/* Protocol symbols. */
#define PROTO_HTTP 0
#define PROTO_HTTPS 1
#ifdef USE_SSL
#endif

#ifdef USE_AUTH

static char b64_encode_table[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',  /* 0-7 */
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',  /* 8-15 */
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',  /* 16-23 */
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',  /* 24-31 */
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',  /* 32-39 */
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',  /* 40-47 */
    'w', 'x', 'y', 'z', '0', '1', '2', '3',  /* 48-55 */
    '4', '5', '6', '7', '8', '9', '+', '/'   /* 56-63 */
};

/* Do base-64 encoding on a hunk of bytes.   Return pointer to the
 ** bytes generated.  Base-64 encoding takes up 4/3 the space of the original,
 ** plus a bit for end-padding.  3/2+5 gives a safe margin.
 */
static char * b64_encode(unsigned const char* ptr, long len) {
    char * space;
    int ptr_idx;
    int c = 0;
    int d = 0;
    int space_idx = 0;
    int phase = 0;

    /*FIXME calculate the occupied space properly*/
    size_t size = ((size_t)len * 3) /2 + 5;
    space = malloc(size+1);
    space[size] = 0;

    for (ptr_idx = 0; ptr_idx < len; ++ptr_idx) {
        switch (phase++) {
            case 0:
                c = ptr[ptr_idx] >> 2;
                d = (ptr[ptr_idx] & 0x3) << 4;
                break;
            case 1:
                c = d | (ptr[ptr_idx] >> 4);
                d = (ptr[ptr_idx] & 0xf) << 2;
                break;
            case 2:
                c = d | (ptr[ptr_idx] >> 6);
                if (space_idx < size) space[space_idx++] = b64_encode_table[c];
                c = ptr[ptr_idx] & 0x3f;
                break;
        }
        space[space_idx++] = b64_encode_table[c];
        if (space_idx == size) return space;
        phase %= 3;
    }
    if (phase != 0) {
        space[space_idx++] = b64_encode_table[d];
        if (space_idx == size) return space;
        /* Pad with ='s. */
        while (phase++ > 0) {
            space[space_idx++] = '=';
            if (space_idx == size) return space;
            phase %= 3;
        }
    }
    return space;
}

#endif /* USE_AUTH */

/*
 * The FUSE operations originally ripped from the hello_ll sample.
 */

static int httpfs_stat(fuse_ino_t ino, struct stat *stbuf)
{
    stbuf->st_ino = ino;
    switch (ino) {
        case 1:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;

        case 2: {
                    struct_url * url = thread_setup();
                    stbuf->st_mode = S_IFREG | 0444;
                    stbuf->st_nlink = 1;
                    return (int) get_stat(url, stbuf);
                }; break;

        default:
            errno = ENOENT;
            return -1;
    }
    return 0;
}

static void httpfs_getattr(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    struct stat stbuf;

    (void) fi;

    memset(&stbuf, 0, sizeof(stbuf));
    if (httpfs_stat(ino, &stbuf) < 0)
        assert(errno),fuse_reply_err(req, errno);
    else
        fuse_reply_attr(req, &stbuf, 1.0);
}

static void httpfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    if (parent != 1 || strcmp(name, main_url.name) != 0){
        e.ino = 0;
    } else {
        e.ino = 2;
        if(httpfs_stat(e.ino, &e.attr) < 0){
            assert(errno);
            fuse_reply_err(req, errno);
            return;
        }

    }
    fuse_reply_entry(req, &e);
}

struct dirbuf {
    char *p;
    size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
        fuse_ino_t ino)
{
    struct stat stbuf;
    size_t oldsize = b->size;
    b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    b->p = (char *) realloc(b->p, b->size);
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;
    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
            (off_t) b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
        off_t off, size_t maxsize)
{
    assert(off >= 0);

    if (off < bufsize)
        return fuse_reply_buf(req, buf + off,
                min(bufsize - (size_t)off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

static void httpfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
        off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    if (ino != 1)
        fuse_reply_err(req, ENOTDIR);
    else {
        struct dirbuf b;

        memset(&b, 0, sizeof(b));
        dirbuf_add(req, &b, ".", 1);
        dirbuf_add(req, &b, "..", 1);
        dirbuf_add(req, &b, main_url.name, 2);
        reply_buf_limited(req, b.p, b.size, off, size);
        free(b.p);
    }
}

static void httpfs_open(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    if (ino != 2)
        fuse_reply_err(req, EISDIR);
    else if ((fi->flags & 3) != O_RDONLY)
        fuse_reply_err(req, EACCES);
    else{
        /* direct_io is supposed to allow partial reads. However, setting
         * the flag causes read length max at 4096 bytes which leads to
         * *many* requests, poor performance, and errors. Some resources
         * like TCP ports are recycled too fast for Linux to cope.
         */
        //fi->direct_io = 1;
        fuse_reply_open(req, fi);
    }
}

static void httpfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
        off_t off, struct fuse_file_info *fi)
{
    (void) fi;

    struct_url * url = thread_setup();
    ssize_t res;

    assert(ino == 2);

    assert(url->file_size >= off);

    size=min(size, (size_t)(url->file_size - off));

    if(url->file_size == off) {
        /* Handling of EOF is not well documented, returning EOF as error
         * does not work but this does.  */
        fuse_reply_buf(req, NULL,  0);
        return;
    }
    /* since we have to return all stuff requested the buffer cannot be
     * allocated in advance */
    if(url->req_buf
            && ( (url->req_buf_size < size )
                || ( (url->req_buf_size > size )
                    && (url->req_buf_size > MAX_REQUEST) ) ) ){
        free(url->req_buf);
        url->req_buf = 0;
    }
    if(! url->req_buf){
        url->req_buf_size = size;
        url->req_buf = malloc(size);
    }

    if((res = get_data(url, off, size)) < 0){
        assert(errno);
        fuse_reply_err(req, errno);
    }else{
        fuse_reply_buf(req, url->req_buf, (size_t)res);
    }
}

static struct fuse_lowlevel_ops httpfs_oper = {
    .lookup             = httpfs_lookup,
    .getattr            = httpfs_getattr,
    .readdir            = httpfs_readdir,
    .open               = httpfs_open,
    .read               = httpfs_read,
};

/*
 * A few utility functions
 */
#ifdef NEED_STRNDUP
static char * strndup(const char * str, size_t n){
    if(n > strlen(str)) n = strlen(str);
    char * res = malloc(n + 1);
    memcpy(res, str, n);
    res[n] = 0;
    return res;
}
#endif

static int mempref(const char * mem, const char * pref, size_t size)
{
    /* return true if found */
   if (size < strlen(pref)) return 0;
   return ! memcmp(mem, pref, strlen(pref));
}

static void errno_report(const char * where)
{
    int e = errno;
    fprintf(stderr, "%s: %s: %d %s.\n", argv0, where, errno, strerror(errno));
    errno = e;
}

static char * url_encode(char * path) {
    return strdup(path); /*FIXME encode*/
}

/*
 * functions for handling struct_url
 */

static int init_url(struct_url* url)
{
    memset(url, 0, sizeof(url));
    url->sock_type = SOCK_CLOSED;
    url->timeout = TIMEOUT;
    return 0;
}

static int free_url(struct_url* url)
{
    if(url->host) free(url->host);
    url->host = 0;
    if(url->path) free(url->path);
    url->path = 0;
    if(url->name) free(url->name);
    url->name = 0;
#ifdef USE_AUTH
    if(url->auth) free(url->auth);
    url->auth = 0;
#endif
    if(url->sock_type != SOCK_CLOSED)
        close_client_force(url);
    url->port = 0;
    url->proto = 0; /* only after socket closed */
    url->file_size=0;
    url->last_modified=0;
    return 0;
}

static void print_url(FILE *f, const struct_url * url)
{
    char * protocol = "?!?";
    switch(url->proto){
        case PROTO_HTTP:
            protocol = "http";
            break;;
        case PROTO_HTTPS:
            protocol = "https";
            break;;
    }
    fprintf(f, "file name: \t%s\n", url->name);
    fprintf(f, "host name: \t%s\n", url->host);
    fprintf(f, "port number: \t%d\n", url->port);
    fprintf(f, "protocol: \t%s\n", protocol);
    fprintf(f, "request path: \t%s\n", url->path);
#ifdef USE_AUTH
    fprintf(f, "auth data: \t%s\n", url->auth ? "(present)" : "(null)");
#endif
}

static int parse_url(const char * url, struct_url* res)
{
    const char * url_orig = url;
    char* http = "http://";
#ifdef USE_SSL
    char* https = "https://";
#endif /* USE_SSL */
    int path_start = '/';
    assert(url);

    if (strncmp(http, url, strlen(http)) == 0) {
        url += strlen(http);
        res->proto = PROTO_HTTP;
        res->port = 80; 
#ifdef USE_SSL
    } else if (strncmp(https, url, strlen(https)) == 0) {
        url += strlen(https);
        res->proto = PROTO_HTTPS;
        res->port = 443;
#endif /* USE_SSL */
    } else {
        fprintf(stderr, "Invalid protocol in url: %s\n", url_orig);
        return -1;
    }

    /* determine if path was given */
    if(strchr(url, path_start))
        res->path = url_encode(strchr(url, path_start));
    else{
        path_start = 0;
        res->path = strdup("/");
    }


#ifdef USE_AUTH
    /* Get user and password */
    if(strchr(url, '@') && (strchr(url, '@') < strchr(url, path_start))){
        res->auth = b64_encode((unsigned char *)url, strchr(url, '@') - url);
        url = strchr(url, '@') + 1;
    }else{
        res->auth = 0;
    }
#endif /* USE_AUTH */

    /* Get port number. */
    int host_end = path_start;
    if(strchr(url, ':') && (strchr(url, ':') < strchr(url, path_start))){
        /* FIXME check that port is a valid numeric value */
        res->port = atoi(strchr(url, ':') + 1);
        if (! res->port) {
            fprintf(stderr, "Invalid port in url: %s\n", url_orig);
            return -1;
        }
        host_end = ':';
    }
    /* Get the host name. */
    if (url == strchr(url, host_end)){ /*no hastname in the url */
        fprintf(stderr, "No hostname in url: %s\n", url_orig);
        return -1;
    }
    res->host = strndup(url, (size_t)(strchr(url, host_end) - url));

    /* Get the file name. */
    url = strchr(url, path_start);
    const char * end = url + strlen(url);
    end--;

    /* Handle broken urls with multiple slashes. */
    while((end > url) && (*end == '/')) end--;
    end++;
    if((path_start == 0) || (end == url)
            || (strncmp(url, "/", (size_t)(end - url)) ==  0)){
        res->name = strdup(res->host);
    }else{
        while(strchr(url, '/') && (strchr(url, '/') < end))
            url = strchr(url, '/') + 1;
        res->name = strndup(url, (size_t)(end - url));
    }

    return res->proto;
}

static void usage(void)
{
        fprintf(stderr, "%s >>> Version: %s <<<\n", __FILE__, VERSION);
        fprintf(stderr, "usage:  %s [-c [console]] [-f] [-t timeout] [-r] url mount-parameters\n\n", argv0);
        fprintf(stderr, "\t -c \tuse console for standard input/output/error (default: %s)\n", CONSOLE);
        fprintf(stderr, "\t -f \tstay in foreground - do not fork\n");
#ifdef RETRY_ON_RESET
        fprintf(stderr, "\t -r \tretry connection on reset\n");
#endif
        fprintf(stderr, "\t -t \tset socket timeout in seconds (default: %i)\n", TIMEOUT);
        fprintf(stderr, "\tmount-parameters should include the mount point\n");
}

#define shift { if(!argv[1]) { usage(); return 4; };\
    argc--; argv[1] = argv[0]; argv = argv + 1;}

int main(int argc, char *argv[])
{
    char * fork_terminal = CONSOLE;
    int do_fork = 1;
    putenv("TZ=");/*UTC*/
    argv0 = argv[0];
    init_url(&main_url);

    while( argv[1] && (*(argv[1]) == '-') )
    {
        char * arg = argv[1]; shift;
        while (*++arg){
            switch (*arg){
                case 'c': if( *(argv[1]) != '-' ) {
                              fork_terminal = argv[1]; shift;
                          }else{
                              fork_terminal = 0;
                          }
                          break;
#ifdef RETRY_ON_RESET
                case 'r': main_url.retry_reset = 1;
                          break;
#endif
                case 't': {
                              char * end = " ";
                              if( isdigit(*(argv[1]))) {
                                  main_url.timeout = strtol(argv[1], &end, 0);
                                  /* now end should point to '\0' */
                              }
                              if(*end){
                                  usage();
                                  fprintf(stderr, "'%s' is not a number.\n",
                                          argv[0]);
                                  return 4;
                              }
                              shift;
                          }
                          break;
                case 'f': do_fork = 0;
                          break;
                default:
                          usage();
                          fprintf(stderr, "Unknown option '%c'.\n", *arg);
                          return 4;
            }
        }
    }

    if (argc < 3) {
        usage();
        return 1;
    }
    if(parse_url(argv[1], &main_url) == -1){
        fprintf(stderr, "invalid url: %s\n", argv[1]);
        return 2;
    }
    print_url(stderr, &main_url);
    int sockfd = open_client_socket(&main_url);
    if(sockfd < 0) {
        fprintf(stderr, "Connection failed.\n");
        return 3;
    }
    close_client_socket(&main_url);
    struct stat st;
    off_t size = get_stat(&main_url, &st);
    if(size >= 0) {
        fprintf(stderr, "file size: \t%" PRIdMAX "\n", (intmax_t)size);
    }else{
        return 3;
    }

    shift;
    if(fork_terminal && access(fork_terminal, O_RDWR)){
        errno_report(fork_terminal);
        fork_terminal=0;
    }

#ifdef USE_THREAD
    close_client_force(&main_url); /* each thread should open its own socket */
    pthread_key_create(&url_key, &destroy_url_copy);
#endif
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;
    char *mountpoint;
    int err = -1;
    int fork_res = 0;

    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
            (ch = fuse_mount(mountpoint, &args)) != NULL) {
        /* try to fork at some point where the setup is mostly done */
        /* FIXME try to close std* and the like ? */
        if(do_fork) fork_res = fork();

        switch (fork_res) {
            case 0:

                {
                    if(fork_terminal){
                        /* if we can access the console use it */
                        int fd = open(fork_terminal, O_RDONLY);
                        dup2(fd, 0);
                        close (fd);
                        fd = open(fork_terminal, O_WRONLY);
                        dup2(fd, 1);
                        close (fd);
                        fd = open(fork_terminal, O_WRONLY|O_SYNC);
                        dup2(fd, 2);
                        close (fd);
                    }

                    struct fuse_session *se;
                    se = fuse_lowlevel_new(&args, &httpfs_oper,
                            sizeof(httpfs_oper), NULL);
                    if (se != NULL) {
                        if (fuse_set_signal_handlers(se) != -1) {
                            fuse_session_add_chan(se, ch);
                            err = FUSE_LOOP(se);
                            fuse_remove_signal_handlers(se);
                            fuse_session_remove_chan(ch);
                        }
                        fuse_session_destroy(se);
                    }
                    fuse_unmount(mountpoint, ch);
                }
                break;;
            case -1:
                errno_report("fork");
                break;;
            default:
                err = 0;
                break;;
        }
    }
    fuse_opt_free_args(&args);

    return err ? err : 0;
}



/*
 * Socket operations that abstract ssl and keepalive as much as possible.
 * Keepalive is set when parsing the headers.
 *
 */

static int close_client_socket(struct_url *url) {
    if (url->sock_type == SOCK_KEEPALIVE) return SOCK_KEEPALIVE;
    return close_client_force(url);
}

static int close_client_force(struct_url *url) {
    if(url->sock_type != SOCK_CLOSED){
#ifdef USE_SSL
        if (url->proto == PROTO_HTTPS) {
            SSL_free(url->ssl);
            SSL_CTX_free(url->ssl_ctx);
            /* FIXME ssl errors
             * The openssl documentation says they can be collected here but
             * they are probably seen as read/write errors anyway.. */
        }
#endif
        close(url->sockfd);
    }
    return url->sock_type = SOCK_CLOSED;
}

#ifdef USE_THREAD

static void destroy_url_copy(void * urlptr)
{
    if(urlptr){
        fprintf(stderr, "%s: Thread %08lX ended.\n", argv0, pthread_self());
        close_client_force(urlptr);
        free(urlptr);
    }
}

static void * create_url_copy(const struct_url * url)
{
    void * res = malloc(sizeof(struct_url));
    memcpy(res, url, sizeof(struct_url));
    return res;
}

static struct_url * thread_setup(void)
{
    struct_url * res = pthread_getspecific(url_key);
    if(!res) {
        fprintf(stderr, "%s: Thread %08lX started.\n", argv0, pthread_self());
        res = create_url_copy(&main_url);
        pthread_setspecific(url_key, res);
    }
    return res;
}

#else /*USE_THREAD*/
static struct_url * thread_setup(void) { return &main_url; }
#endif


static ssize_t read_client_socket(struct_url *url, void * buf, size_t len) {
    ssize_t res;
    struct timeval timeout;
    timeout.tv_sec = url->timeout;
    timeout.tv_usec = 0;
    setsockopt(url->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef USE_SSL
    if (url->proto == PROTO_HTTPS)
        res = SSL_read(url->ssl, buf, len);
    else
#endif
        res = read(url->sockfd, buf, len);
    if(res <= 0) errno_report("read");
    return res;
}

static ssize_t
write_client_socket(struct_url *url, const void * buf, size_t len)
{
    do {
        int fd = open_client_socket(url);
        ssize_t res;

        if(fd < 0) return -1; /*error hopefully reported by open*/
#ifdef USE_SSL
        if (url->proto == PROTO_HTTPS)
            res =  SSL_write(url->ssl, buf, len);
        else
#endif
            res = write(url->sockfd, buf, len);
        if(res <= 0) errno_report("write");
        if ( !(res <= 0) || (url->sock_type != SOCK_KEEPALIVE )) return res;

        /* retry a failed keepalive socket */
        close_client_force(url);
    } while (url->sock_type == SOCK_KEEPALIVE);
    return -1; /*should not reach*/
}

/*
 * Function yields either a positive int after connecting to
 * host 'hostname' on port 'port'  or < 0 in case of error
 *
 * It handles keepalive by not touching keepalive sockets.
 * The SSL context is created so that read/write can use it.
 *
 * hostname is something like 'www.tmtd.de' or 192.168.0.86
 * port is expected in machine order (not net order)
 *
 * ((Flonix  defines USE_IPV6))
 *
 */
#if defined(AF_INET6) && defined(IN6_IS_ADDR_V4MAPPED)
#define USE_IPV6
#endif

static int open_client_socket(struct_url *url) {
#ifdef USE_IPV6
    struct addrinfo hints;
    char portstr[10];
    int gaierr;
    struct addrinfo* ai;
    struct addrinfo* aiv4;
    struct addrinfo* aiv6 = 0;
    struct sockaddr_in6 sa;
#else /* USE_IPV6 */
    struct hostent *he;
    struct sockaddr_in sa;
#endif /* USE_IPV6 */
    socklen_t sa_len;
    int sock_family, sock_type, sock_protocol;

    if(url->sock_type == SOCK_KEEPALIVE) return url->sock_type;
    if(url->sock_type != SOCK_CLOSED) close_client_socket(url);

    (void) memset((void*) &sa, 0, sizeof(sa));

#ifdef USE_IPV6
    (void) memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void) snprintf(portstr, sizeof(portstr), "%d", (int) url->port);
    if ((gaierr = getaddrinfo(url->host, portstr, &hints, &ai)) != 0) {
        (void) fprintf(stderr, "%s: getaddrinfo %s - %s\n",
                argv0, url->host, gai_strerror(gaierr));
        return -1;
    }

    /* Find the first IPv4 and IPv6 entries. */
    for (aiv4 = ai; aiv4 != NULL; aiv4 = aiv4->ai_next) {
        if (aiv4->ai_family == AF_INET)
            break;
        if ((aiv4->ai_family == AF_INET6) && (aiv6 == NULL))
            aiv6 = aiv4;
    }

    /* If there's an IPv4 address, use that, otherwise try IPv6. */
    if (aiv4 == NULL)
        aiv4 = aiv6;
    if (aiv4 == NULL) {
        (void) fprintf(stderr, "%s: no valid address found for host %s\n",
                argv0, url->host);
        errno = EIO;
        return -1;
    }
    if (sizeof(sa) < aiv4->ai_addrlen) {
        (void) fprintf(stderr, "%s - sockaddr too small (%lu < %lu)\n",
                url->host, (unsigned long) sizeof(sa),
                (unsigned long) aiv4->ai_addrlen);
        errno = EIO;
        return -1;
    }
    sock_family = aiv4->ai_family;
    sock_type = aiv4->ai_socktype;
    sock_protocol = aiv4->ai_protocol;
    sa_len = aiv4->ai_addrlen;
    (void) memmove(&sa, aiv4->ai_addr, sa_len);
    freeaddrinfo(ai);

#else /* USE_IPV6 */

    he = gethostbyname(url->host);
    if (he == NULL) {
        (void) fprintf(stderr, "%s: unknown host - %s\n", argv0, url->host);
        errno = EIO;
        return -1;
    }
    sock_family = sa.sin_family = he->h_addrtype;
    sock_type = SOCK_STREAM;
    sock_protocol = 0;
    sa_len = sizeof(sa);
    (void) memmove(&sa.sin_addr, he->h_addr, he->h_length);
    sa.sin_port = htons(url->port);

#endif /* USE_IPV6 */

    url->sockfd = socket(sock_family, sock_type, sock_protocol);
    if (url->sockfd < 0) {
        errno_report("couldn't get socket");

        return -1;
    }
    if (connect(url->sockfd, (struct sockaddr*) &sa, sa_len) < 0) {
        errno_report("couldn't connect socket");
        return -1;
    }

#ifdef USE_SSL
    if ((url->proto) == PROTO_HTTPS) {
        /* Make SSL connection. */
        int r;
        SSL_load_error_strings();
        SSLeay_add_ssl_algorithms();
        url->ssl_ctx = SSL_CTX_new(SSLv23_client_method());
        url->ssl = SSL_new(url->ssl_ctx);
        SSL_set_fd(url->ssl, url->sockfd);
        r = SSL_connect(url->ssl);
        if (r <= 0) {
            close(url->sockfd);
            (void) fprintf(stderr, "%s: %s:%d - SSL connection failed - %d\n",
                    argv0, url->host, url->port, r);
            ERR_print_errors_fp(stderr);
            errno = EIO;
            return -1;
        }
    }
#endif
    return url->sock_type = SOCK_OPEN;
}

static void
plain_report(const char * reason, const char * method,
        const char * buf, size_t len)
{
    fprintf(stderr, "%s: %s: %s\n", argv0, method, reason);
    fwrite(buf, len, 1, stderr);
    if(len && ( *(buf+len-1) != '\n')) fputc('\n', stderr);
}

/*
 * Scan the received header for interesting fields. Since C does not have
 * tools for working with potentially unterminated strings this is quite
 * long and ugly.
 *
 * Return the length of the header in case part of the data was
 * read with the header.
 * Content-Length means different thing whith GET and HEAD.
 */

static ssize_t
parse_header(struct_url *url, const char * buf, size_t bytes,
        const char * method, off_t * content_length, int expect)
{
    /* FIXME check the header parser */
    int status;
    const char * ptr = buf;
    const char * end;
    int seen_accept = 0, seen_length = 0, seen_close = 0;

    if (bytes <= 0) {
        return -1;
    }

    end = memchr(ptr, '\n', bytes);
    if(!end) {
        plain_report ( "reply does not contain newline!", method, buf, 0);
        errno = EIO;
        return -1;
    }
    end = ptr;
    while(1){
        end = memchr(end + 1, '\n', bytes - (size_t)(end - ptr));
        if(!end || ((end + 1) >= (ptr + bytes)) ) {
            plain_report ("reply does not contain end of header!",
                    method, buf, bytes);
            errno = EIO;
            return -1;
        }
        if(mempref(end, "\n\r\n", bytes - (size_t)(end - ptr))) break;
    }
    ssize_t header_len = (end + 3) - ptr;

    end = memchr(ptr, '\n', bytes);
    char * http = "HTTP/1.1 ";
    if(!mempref(ptr, http, (size_t)(end - ptr)) || !isdigit( *(ptr + strlen(http))) ) {
        plain_report ("reply does not contain status!",
                method, buf, (size_t)header_len);
        errno = EIO;
        return -1;
    }
    status = (int)strtol( ptr + strlen(http), (char **)&ptr, 10);
    if (status != expect) {
        fprintf(stderr, "%s: %s: failed with status: %d%.*s.\n",
                argv0, method, status, (int)((end - ptr) - 1), ptr);
        if (!strcmp("HEAD", method)) fwrite(buf, bytes, 1, stderr); /*DEBUG*/
        errno = EIO;
        if (status == 404) errno = ENOENT;
        return -1;
    }

    char * content_length_str = "Content-Length: ";
    char * accept = "Accept-Ranges: bytes";
    char * date = "Last-Modified: ";
    char * close = "Connection: close";
    struct tm tm;
    while(1)
    {
        ptr = end+1;
        if( !(ptr < buf + (header_len - 4))){
            if(! seen_accept){
                plain_report("server must Accept-Range: bytes",
                        method, buf, 0);
                errno = EIO;
                return -1;
            }
            if(! seen_length){
                plain_report("reply didn't contain Content-Length!",
                        method, buf, 0);
                errno = EIO;
                return -1;
            }
            if(url->sock_type == SOCK_OPEN && !seen_close)
                url->sock_type = SOCK_KEEPALIVE;
            if(url->sock_type == SOCK_KEEPALIVE && seen_close)
                url->sock_type = SOCK_OPEN;
            return header_len;
        }
        end = memchr(ptr, '\n', bytes - (size_t)(ptr - buf));
        if( mempref(ptr, content_length_str, (size_t)(end - ptr))
                && isdigit( *(ptr + strlen(content_length_str))) ){
            *content_length = atoll(ptr + strlen(content_length_str));
            seen_length = 1;
            continue;
        }
        if( mempref(ptr, accept, (size_t)(end - ptr)) ){
            seen_accept = 1;
            continue;
        }
        if( mempref(ptr, date, (size_t)(end - ptr)) ){
            memset(&tm, 0, sizeof(tm));
            if(!strptime(ptr + strlen(date),
                        "%n%a, %d %b %Y %T %Z", &tm)){
                plain_report("invalid time",
                        method, ptr + strlen(date),
                        (size_t)(end - ptr) - strlen(date)) ;
                continue;
            }
            url->last_modified = mktime(&tm);
            continue;
        }
        if( mempref(ptr, close, (size_t)(end - ptr)) ){
            seen_close = 1;
        }
    }
}

/*
 * Send the header, and get a reply.
 * This relies on 1k reads and writes being generally atomic -
 * - they fit into a single frame. The header should fit into that
 * and we do not need partial read handling so the exchange is simple.
 * However, broken sockets have to be handled here.
 */

static ssize_t
exchange(struct_url *url, char * buf, const char * method,
        off_t * content_length, off_t start, off_t end, size_t * header_length)
{
    ssize_t res;
    size_t bytes;
    int range = (end > 0);

    /* Build request buffer, starting with the request method. */

    bytes = (size_t)snprintf(buf, HEADER_SIZE, "%s %s HTTP/1.1\r\nHost: %s\r\n",
            method, url->path, url->host);
    bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
            "User-Agent: %s %s\r\n", __FILE__, VERSION);
    if (range) bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
                "Range: bytes=%" PRIdMAX "-%" PRIdMAX "\r\n", (intmax_t)start, (intmax_t)end);
#ifdef USE_AUTH
    if ( url->auth )
        bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes,
                "Authorization: Basic %s\r\n", url->auth);
#endif
    bytes += (size_t)snprintf(buf + bytes, HEADER_SIZE - bytes, "\r\n");


    /* Now actually send it. */
    while(1){
        /*
         * It looks like the sockets abandoned by the server do not go away.
         * Instead of returning EPIPE they allow zero writes and zero reads. So
         * this is the place where a stale socket would be detected.
         *
         * Socket that return EAGAIN cause long delays. Reopen.
         *
         * Reset errno because reads/writes of 0 bytes are a success and are not
         * required to touch it but are handled as error below.
         *
         */
        /* ECONNRESET happens with some dodgy servers so may need to handle that.
         * Allow for building without it in case it is not defined.
         */
#ifdef RETRY_ON_RESET
#define CONNFAIL ((res <= 0) && ! errno) || (errno == EAGAIN) || (errno == EPIPE) || \
        (url->retry_reset && (errno == ECONNRESET))
#else
#define CONNFAIL ((res <= 0) && ! errno) || (errno == EAGAIN) || (errno == EPIPE)
#endif
        errno = 0;
        res = write_client_socket(url, buf, bytes);
        if (CONNFAIL) {
            errno_report("exchange: failed to send request, retrying"); /* DEBUG */
            close_client_force(url);
            continue;
        }
        if (res <= 0){
            errno_report("exchange: failed to send request"); /* DEBUG */
            return res;
        }
        res = read_client_socket(url, buf, HEADER_SIZE);
        if (CONNFAIL) {
            errno_report("exchange: did not receive a reply, retrying"); /* DEBUG */
            close_client_force(url);
            continue;
        } else if (res <= 0) {
            errno_report("exchange: failed receving reply from server"); /* DEBUG */
            return res;
        } else break;
        /* Not reached */
    }
    bytes = (size_t)res;

    res = parse_header(url, buf, bytes, method, content_length,
            range ? 206 : 200);
    if (res <= 0){
        plain_report("exchange: server error", method, buf, bytes);
        return res;
    }

    if (header_length) *header_length = (size_t)res;

    return (ssize_t)bytes;
}

/*
 * Function uses HEAD-HTTP-Request
 * to determine the file size
 */

static off_t get_stat(struct_url *url, struct stat * stbuf) {
    char buf[HEADER_SIZE];

    if( exchange(url, buf, "HEAD", &(url->file_size), 0, 0, 0) < 0 )
        return -1;

    close_client_socket(url);

    stbuf->st_mtime = url->last_modified;
    return stbuf->st_size = url->file_size;
}

/*
 * get_data does all the magic
 * a GET-Request with Range-Header
 * allows to read arbitrary bytes
 */

static ssize_t get_data(struct_url *url, off_t start, size_t size)
{
    char buf[HEADER_SIZE];
    const char * b;
    ssize_t bytes;
    off_t end = start + (off_t)size - 1;
    char * destination = url->req_buf;
    off_t content_length;
    size_t header_length;

    bytes = exchange(url, buf, "GET", &content_length,
            start, end, &header_length);
    if(bytes <= 0) return -1;

    if (content_length != size) {
        plain_report("didn't yield the whole piece.", "GET", 0, 0);
        size = min((size_t)content_length, size);
    }


    b = buf + header_length;

    bytes -= (b - buf);
    memcpy(destination, b, (size_t)bytes);
    size -= (size_t)bytes;
    destination +=bytes;
    for (; size > 0; size -= (size_t)bytes, destination += bytes) {

        bytes = read_client_socket(url, destination, size);
        if (bytes < 0) {
            errno_report("GET (read)");
            return -1;
        }
        if (bytes == 0) {
            break;
        }
    }

    close_client_socket(url);

    return (ssize_t)(end - start) + 1 - (ssize_t)size;
}
