#include <listener.h>
#include <b64/b64.h>
#include <string.h>
#include <proxy.h>
#include <zmq.h>
#include <task/task.h>
#include <dbg.h>
#include <handler.h>
#include <dir.h>
#include <request.h>
#include <register.h>
#include <pattern.h>


char *FLASH_RESPONSE = "<?xml version=\"1.0\"?><!DOCTYPE cross-domain-policy SYSTEM \"http://www.macromedia.com/xml/dtds/cross-domain-policy.dtd\"> <cross-domain-policy> <allow-access-from domain=\"*\" to-ports=\"*\" /></cross-domain-policy>";

size_t FLASH_LEN = 0;

char *HTTP_404 = "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "Content-Length: 9\r\n"
    "Server: Mongrel2\r\n\r\nNot Found";

const char *PING_PATTERN = "@[a-z/]- {\"type\":%s*\"ping\"}"; 


// these are used by unit tests to fake out sockets from files
static int (*Listener_read_func)(int, void*, int);
static int (*Listener_write_func)(int, void*, int);


void Listener_init()
{
    FLASH_LEN = strlen(FLASH_RESPONSE);
    Listener_set_iofuncs(fdrecv, fdsend);
}

void Listener_set_iofuncs( int (*read_func)(int, void*, int),
        int (*write_func)(int, void*, int))
{
    Listener_read_func = read_func;
    Listener_write_func = write_func;
}


void Listener_accept(Server *srv, int fd, int rport, const char *remote)
{
    Listener *listener = Listener_create(srv, fd, rport, remote);

    taskcreate(Listener_task, listener, LISTENER_STACK);
}


Listener *Listener_create(Server *srv, int fd, int rport, const char *remote)
{
    Listener *listener = calloc(sizeof(Listener), 1);
    check(listener, "Out of memory.");

    listener->server = srv;
    listener->fd = fd;

    listener->rport = rport;
    strncpy(listener->remote, remote, IPADDR_SIZE);
    listener->remote[IPADDR_SIZE] = '\0';

    listener->parser = Request_create();
    check(listener->parser, "Failed to allocate Request.");


    return listener;
error:
    Listener_destroy(listener);
    return NULL;
}

void Listener_destroy(Listener *listener)
{
    if(listener) {
        Request_destroy(listener->parser);
        free(listener);
    }
}


int Listener_deliver(int to_fd, char *buffer, size_t len)
{
    size_t b64_len = 0;
    b64_char_t *b64_buf = NULL;
    int rc = 0;

    check(buffer[len] == '\0', "Message for listener must end in \\0, you have '%c'", buffer[len]);
    check(buffer[len-1] != '\0', "Message for listener must end in ONE \\0, you have more.");

    b64_buf = calloc(BUFFER_SIZE * 1.5, 1);
    check(b64_buf, "Failed to allocate buffer for Base64 convert.");

    b64_len = b64_encode(buffer, len, b64_buf, BUFFER_SIZE * 1.5-1);
    check(b64_len > 0, "Base64 convert failed.");

    b64_buf[b64_len] = '\0';

    rc = Listener_write_func(to_fd, b64_buf, b64_len+1);
    check(rc == b64_len+1, "Failed to write entire message to listener %d", to_fd);


    if(b64_buf) free(b64_buf);
    return 0;

error:
    if(b64_buf) free(b64_buf);
    return -1;
}



int Listener_process_json(Listener *listener)
{
    check(listener->server->default_host, "No default host set, need one for jssockets to work.");

    if(!listener->registered) {
        Register_connect(listener->fd);
        listener->registered = 1;
    }

    if(pattern_match(listener->buf, listener->parser->body_start, PING_PATTERN)) {
        if(!Register_ping(listener->fd)) {
            Register_disconnect(listener->fd);
        }

    } else {
        const char *path = NULL;
        Backend *found = Listener_match_path(listener, &path);
        check(found, "Handler not found: %s", path);
        check(found->type == BACKEND_HANDLER, "Should get a handler.");

        debug("JSON message from %s:%d sent on jssocket: %.*s", listener->remote, listener->rport, listener->nread, listener->buf);

        if(Handler_deliver(found->target.handler->send_socket, listener->fd, listener->buf, listener->nread) == -1) {
            log_err("Can't deliver message to handler.");
       }
    }

    return 0;
error:
    return -1;
}

Backend *Listener_match_path(Listener *listener, const char **out_path)
{
    *out_path = Request_get(listener->parser, "PATH");
    check(*out_path, "Invalid HTTP Request, no PATH parameter.");

    Backend *found = Host_match(listener->server->default_host, *out_path, strlen(*out_path));

    return found;

error:
    return NULL;
}

int Listener_process_http(Listener *listener)
{
    // TODO: resolve what the default host means, maybe we're matching hosts now?
    check(listener->server->default_host, "No default host set.");

    taskname("Listener_task");

    const char *path = NULL;

    Backend *found = Listener_match_path(listener, &path);
    check(path, "PATH not given, invalid HTTP request.");

    
    if(found) {
        // we can share the data because the caller will block as the proxy runs
        switch(found->type) {
            case BACKEND_PROXY:
                taskstate("proxying");
                return Proxy_connect(found->target.proxy, listener->fd, listener->buf, BUFFER_SIZE, listener->nread);

            case BACKEND_HANDLER:
                taskstate("error");
                sentinel("Handler isn't supported for HTTP yet.");
            case BACKEND_DIR:
                taskstate("sending");
                return Dir_serve_file(found->target.dir, path, listener->fd);
            default:
                sentinel("Unknown handler type id: %d", found->type);
        }
    } else {
        // TODO: make a generic error response system
        log_err("[%s] 404 Not Found", path);
        taskstate("404");

        fdwrite(listener->fd, HTTP_404, strlen(HTTP_404));

        taskstate("closing");
        close(listener->fd);

        return -1;
    }
   
error:
    return -1;
}

int Listener_process_flash_socket(Listener *listener)
{
    int rc = Listener_write_func(listener->fd, FLASH_RESPONSE, strlen(FLASH_RESPONSE) + 1);
    check(rc > 0, "Failed to write Flash socket response.");

    return 0;
error:
    return -1;
}

int Listener_parse(Listener *listener)
{
    Request_start(listener->parser);

    int finished = Request_parse(listener->parser, listener->buf,
                        listener->nread, &listener->nparsed);

    check(finished == 1, "Error in parsing: %d, bytes: %d, value: %.*s", 
            finished, listener->nread, listener->nread, listener->buf);

    return 0;
    
error:

    return -1;
}


void Listener_task(void *v)
{
    Listener *listener = (Listener *)v;
    int rc = 0;

    while((listener->nread = Listener_read_func(listener->fd, listener->buf, BUFFER_SIZE-1)) > 0) {
        listener->buf[listener->nread] = '\0';

        rc = Listener_parse(listener);

        Request_dump(listener->parser);

        check(rc == 0, "Parsing failed, closing %s:%d 'cause they suck.",
                listener->remote, listener->rport);
       
        if(listener->parser->socket_started) {
            rc = Listener_process_flash_socket(listener);
            check(rc == 0, "Invalid flash socket, closing %s:%d 'cause flash sucks.",
                    listener->remote, listener->rport);
            break;
        } else if(listener->parser->json_sent) {
            rc = Listener_process_json(listener);
            check(rc == 0, "Invalid json request, closing %s:%d 'cause they can't read.",
                    listener->remote, listener->rport);
        } else {
            rc = Listener_process_http(listener);
            check(rc == 0, "HTTP hand off failed, closing %s:%d",
                    listener->remote, listener->rport);
            break;
        }
    }

error: // fallthrough for both error or not
    if(listener->parser->json_sent) {
        Register_disconnect(listener->fd);
    }

    Listener_destroy(listener);
}


