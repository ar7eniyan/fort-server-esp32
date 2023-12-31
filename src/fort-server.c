#include "fort-server.h"
#include "fort-server_private.h"

#include "esp_log.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

static const char *TAG = "fort-server";

fort_error fort_send_all(int socket, void *buffer, size_t len, int flags)
{
    if (len == 0) return FORT_ERR_OK;
    char *ptr = (char *)buffer;

    while (len) {
        ssize_t i = send(socket, ptr, len, flags);
        if (i < 1) {
            if (i == 0) {
                ESP_LOGE(TAG, "Unexpected socket close");
                return FORT_ERR_SOCKET_CLOSED;
            }
            ESP_LOGE(TAG, "Socket send() error: %s", strerror(errno));
            return FORT_ERR_SEND;
        }
        ptr += i;
        len -= i;
    }
    return FORT_ERR_OK;
}

fort_error fort_recv_all(int socket, void *buffer, size_t len, int flags)
{
    if (len == 0) return FORT_ERR_OK;
    char *ptr = (char *)buffer;

    while (len) {
        ssize_t i = recv(socket, ptr, len, flags);
        if (i < 1) {
            if (i == 0) {
                ESP_LOGE(TAG, "Unexpected socket close");
                return FORT_ERR_SOCKET_CLOSED;
            }
            ESP_LOGE(TAG, "Socket recv() error: %s", strerror(errno));
            return FORT_ERR_RECV;
        }
        ptr += i;
        len -= i;
    }
    return FORT_ERR_OK;
}

#if FORT_EXTRA_DEBUG
const char *fort_state_to_str(fort_state state)
{
    switch (state) {
    case FORT_STATE_IDLE:
        return "IDLE";
    case FORT_STATE_HELLO_SENT:
        return "HELLO-SENT";
    case FORT_STATE_HELLO_RECEIVED:
        return "HELLO-RECEIVED";
    case FORT_STATE_BOUND:
        return "BOUND";
    case FORT_STATE_CLOSING:
        return "CLOSING";
    case FORT_STATE_CLOSED:
        return "CLOSED";
    default:
        return "Not a valid state";
    }
}
#endif

const char *fort_strerror(fort_error err)
{
    switch (err) {
    case FORT_ERR_OK:
        return "Normal operation";
    case FORT_ERR_SOCKET_CLOSED:
        return "Gateway closed service socket";
    case FORT_ERR_RECV:
        return "Error in recv()";
    case FORT_ERR_SEND:
        return "Error in send()";
    case FORT_ERR_GETAI:
        return "Error in getaddrinfo()";
    case FORT_ERR_SOCKET:
        return "Error in socket()";
    case FORT_ERR_CONNECT:
        return "Error in connect()";
    case FORT_ERR_GATEWAY_BIND:
        return "Gateway failed to bind to a requested port";
    case FORT_ERR_TIMEOUT:
        return "Timeout in fort_accept() or in receiving a response to HELLO "
               "or SHUTD";
    case FORT_ERR_WRONG_STATE:
        return "Unexpected session state";
    case FORT_ERR_QUEUE_FULL:
        return "Accept queue is full";
    case FORT_ERR_PROTOCOL:
        return "Protocol error (e.g. invalid packet, etc.)";
    default:
        return "Unknown error";
    }
}

fort_error fort_begin(void)
{
    EXPECT_STATE(&fort_main_session, FORT_STATE_UNITIALIZED);
    fort_main_session.lock = xSemaphoreCreateMutex();
    assert(fort_main_session.lock != NULL && "Could not create mutex");
    fort_main_session.events = xEventGroupCreate();
    assert(fort_main_session.events != NULL && "Could not create event group");

    fort_main_session.state = FORT_STATE_IDLE;
    BaseType_t success =
        xTaskCreate(fort_task, FORT_TASK_NAME, FORT_TASK_STACK, NULL,
                    FORT_TASK_PRIO, &fort_globals.fort_task);
    assert(success == pdPASS && "Could not create main task");

    return FORT_ERR_OK;
}

// fort_connect(), fort_bind_and_listen() and fort_disconnect() look like a
// lot of boilerplate code, should(can) I fix it?
fort_error fort_connect(const char *hostname, const uint16_t port)
{
    EXPECT_STATE(&fort_main_session, FORT_STATE_IDLE);
    if (fort_main_session.error) {
        return fort_main_session.error;
    }

    xSemaphoreTake(fort_main_session.lock, portMAX_DELAY);
    xEventGroupClearBits(fort_main_session.events, FORT_EVT_GATEWAY_HELLO);
    fort_error err = fort_do_connect(&fort_main_session, hostname, port);
    fort_main_session.error = (fort_error)err;
    xSemaphoreGive(fort_main_session.lock);
    if (err != FORT_ERR_OK) return err;

    // wait for the gateway to respond with a HELLO
    EventBits_t bits = xEventGroupWaitBits(
        fort_main_session.events, FORT_EVT_GATEWAY_HELLO, pdTRUE, pdTRUE,
        pdMS_TO_TICKS(FORT_REPONSE_TIMEOUT));

    return (bits & FORT_EVT_GATEWAY_HELLO) ? FORT_ERR_OK : FORT_ERR_TIMEOUT;
}

// connect and send hello
fort_error fort_do_connect(fort_session *sess, const char *hostname,
                           const uint16_t port)
{
    int err;
    struct addrinfo hints, *servinfo = NULL;
    struct sockaddr_in gateway_addr;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;      // use IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets

    // resolve the gateway address and put it in servinfo
    if ((err = getaddrinfo(hostname, NULL, &hints, &servinfo)) != 0) {
        // seems like there's no gai_strerror() in LwIP,
        // but errors are defined in esp-lwip/src/include/lwip/netdb.h
        ESP_LOGE(TAG, "getaddrinfo() error: %d", err);
        if (servinfo) freeaddrinfo(servinfo);
        return FORT_ERR_GETAI;
    }

    // use the firt addrinfo entry, fill in the port manually
    memcpy(&gateway_addr, servinfo->ai_addr, servinfo->ai_addrlen);
    gateway_addr.sin_port = htons(port);
    ESP_LOGD(TAG, "Resolved %s to %s", hostname,
             inet_ntoa(gateway_addr.sin_addr.s_addr));

    int service_sock = socket(servinfo->ai_family, servinfo->ai_socktype,
                              servinfo->ai_protocol);
    if (service_sock == -1) {
        ESP_LOGE(TAG, "Socket creation error: %s", strerror(errno));
        freeaddrinfo(servinfo);
        return FORT_ERR_SOCKET;
    }

    err = connect(service_sock, (struct sockaddr *)&gateway_addr,
                  servinfo->ai_addrlen);
    if (err == -1) {
        ESP_LOGE(TAG, "connect() error: %s", strerror(errno));
        close(service_sock);
        freeaddrinfo(servinfo);
        return FORT_ERR_CONNECT;
    }

    // send our HELLO to the gateway
    fort_header hello = {.packet_type = PACKET_HELLO, .data_length = 0};
    if ((err = fort_send_all(service_sock, &hello, sizeof hello, 0)) <
        FORT_ERR_OK) {
        close(service_sock);
        freeaddrinfo(servinfo);
        return (fort_error)err;
    }

    sess->service_socket = service_sock;
    sess->gateway_addr   = gateway_addr;
    sess->state          = FORT_STATE_HELLO_SENT;
    xEventGroupSetBits(sess->events, FORT_EVT_SERVER_HELLO);
    freeaddrinfo(servinfo);
    return FORT_ERR_OK;
}

fort_error fort_bind_and_listen(uint16_t port, int backlog)
{
    EXPECT_STATE(&fort_main_session, FORT_STATE_HELLO_RECEIVED);
    if (fort_main_session.error) {
        return fort_main_session.error;
    }

    xSemaphoreTake(fort_main_session.lock, portMAX_DELAY);
    xEventGroupClearBits(fort_main_session.events, FORT_EVT_GATEWAY_BINDR);
    fort_error err          = fort_do_listen(&fort_main_session, port, backlog);
    fort_main_session.error = (fort_error)err;
    xSemaphoreGive(fort_main_session.lock);

    if (err != FORT_ERR_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(
        fort_main_session.events, FORT_EVT_GATEWAY_BINDR, pdTRUE, pdTRUE,
        pdMS_TO_TICKS(FORT_REPONSE_TIMEOUT));

    if ((bits & FORT_EVT_GATEWAY_BINDR) == 0)
        return FORT_ERR_TIMEOUT;
    else if (fort_main_session.state != FORT_STATE_BOUND)
        return FORT_ERR_GATEWAY_BIND;
    else
        return FORT_ERR_OK;
}

fort_error fort_do_listen(fort_session *sess, const uint16_t port,
                          const int backlog)
{
    sess->accept_queue = xQueueCreate(backlog, sizeof(int));
    assert(sess->accept_queue != NULL && "Failed to create accept queue");
    sess->gateway_bind_port = port;

    fort_header bind = {.packet_type = PACKET_BINDR,
                        .port        = sess->gateway_bind_port,
                        .data_length = 0};
    return fort_send_all(sess->service_socket, &bind, sizeof bind, 0);
}

// If timeout_ms is negative, block forever
int fort_accept(int64_t timeout_ms)
{
    EXPECT_STATE(&fort_main_session, FORT_STATE_BOUND);
    if (fort_main_session.error) {
        return fort_main_session.error;
    }

    int sock;
    TickType_t timeout =
        timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    int rc = xQueueReceive(fort_main_session.accept_queue, &sock, timeout);
    return rc == pdTRUE ? sock : FORT_ERR_TIMEOUT;
}

fort_error fort_disconnect(void)
{
    // Can't use EXPECT_STATE because there are multiple states allowed
    if (fort_main_session.state != FORT_STATE_BOUND &&
        fort_main_session.state != FORT_STATE_HELLO_RECEIVED) {
        ESP_LOGE(TAG,
                 "Unexpected state when trying to disconnect: " STATE_FMT_SPEC,
                 STATE_FMT(fort_main_session.state));
        return FORT_ERR_WRONG_STATE;
    }
    if (fort_main_session.error) {
        return fort_main_session.error;
    }

    xSemaphoreTake(fort_main_session.lock, portMAX_DELAY);
    xEventGroupClearBits(fort_main_session.events, FORT_EVT_GATEWAY_SHUTD);
    fort_error err          = fort_do_disconnect(&fort_main_session);
    fort_main_session.error = err;
    xSemaphoreGive(fort_main_session.lock);

    if (err != FORT_ERR_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(
        fort_main_session.events, FORT_EVT_GATEWAY_SHUTD, pdTRUE, pdTRUE,
        pdMS_TO_TICKS(FORT_REPONSE_TIMEOUT));

    return (bits & FORT_EVT_GATEWAY_SHUTD) ? FORT_ERR_OK : FORT_ERR_TIMEOUT;
}

fort_error fort_do_disconnect(fort_session *sess)
{
    sess->state       = FORT_STATE_CLOSING;
    fort_header shutd = {
        .packet_type = PACKET_SHUTD, .data_length = 0, .port = 0};
    return fort_send_all(sess->service_socket, &shutd, sizeof shutd, 0);
}

fort_error fort_end(void)
{
    EXPECT_STATE(&fort_main_session, FORT_STATE_CLOSED);
    xSemaphoreTake(fort_main_session.lock, portMAX_DELAY);
    fort_error err = fort_do_end(&fort_main_session);
    xSemaphoreGive(fort_main_session.lock);
    return err;
}

fort_error fort_do_end(fort_session *sess)
{
    sess->error             = FORT_ERR_OK;
    sess->state             = FORT_STATE_IDLE;
    sess->gateway_bind_port = 0;
    if (sess->service_socket > 0) close(sess->service_socket);
    sess->service_socket = -1;
    memset(&sess->gateway_addr, 0, sizeof sess->gateway_addr);
    // Clear all the 24 supported bits
    xEventGroupClearBits(sess->events, 0x00FFFFFF);
    if (sess->accept_queue) {
        vQueueDelete(sess->accept_queue);
        sess->accept_queue = NULL;
    }
    return FORT_ERR_OK;
}

fort_error fort_do_close(fort_session *sess)
{
    sess->state = FORT_STATE_CLOSED;
    int err     = close(sess->service_socket);
    if (err != 0) {
        ESP_LOGE(TAG, "close() error: %s", strerror(errno));
    }
    sess->service_socket = -1;
    return FORT_ERR_OK;
}

fort_error fort_clear_error(void)
{
    xSemaphoreTake(fort_main_session.lock, portMAX_DELAY);
    fort_error err          = fort_main_session.error;
    fort_main_session.error = FORT_ERR_OK;
    xSemaphoreGive(fort_main_session.lock);
    return err;
}

fort_state fort_current_state(void) { return fort_main_session.state; }

// Called only from fort-task when there are incoming data on service_socket,
// does not block.
fort_error receive_packet_step(fort_session *sess)
{
    // Can I somehow wait (return) until a full header/data arrives
    // and then read it in a one go?

    fort_error err           = FORT_ERR_OK;
    static size_t bytes_left = sizeof(fort_header);
    static char hdr_buf[sizeof(fort_header)];
    static char *recv_ptr = hdr_buf;
    static char *data_buf;

    if (bytes_left) {
        ssize_t received =
            recv(sess->service_socket, recv_ptr, bytes_left, MSG_DONTWAIT);
        if (received == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FORT_ERR_OK;
            ESP_LOGE(TAG, "recv() error in a main loop: %s", strerror(errno));
        } else {
            bytes_left -= (size_t)received;
            recv_ptr += received;
        }
    }

    // finished receiving packet header
    if (!bytes_left && recv_ptr == hdr_buf + sizeof(fort_header)) {
        bytes_left = ((fort_header *)hdr_buf)->data_length;
        // TODO: use a local buffer for data with length <= 1024 or so
        data_buf   = bytes_left ? (char *)malloc(bytes_left) : NULL;
        // check if malloc succeeds
        assert((!bytes_left || data_buf != NULL) && "malloc failed");
        recv_ptr = data_buf;
    }
    // we've just received packet data or its length is zero after receiving
    // header in both cases the packet is ready for processing
    if (!bytes_left && recv_ptr != hdr_buf + sizeof(fort_header)) {
        xSemaphoreTake(sess->lock, portMAX_DELAY);
        err = handle_packet(sess, (const fort_header *)hdr_buf, data_buf);
        xSemaphoreGive(sess->lock);
        free(data_buf);

        bytes_left = sizeof(fort_header);
        recv_ptr   = hdr_buf;
    }

    return err;
}

//
// Packet handling logic
//

// Implementation of a state machine where the handler function procecces the
// incoming packet, possibly sends a response or closes the connection, sets the
// next state accordingly and returns an error code. Handlers are stored in the
// global state_table 2d array.
// TODO: make a strict mode, where we disconnect on any gateway misbehavior.
fort_error handle_packet(fort_session *sess, const fort_header *hdr,
                         const void *data)
{
    assert(sess->state > 0 && sess->state < FORT_STATE_MAX && "Invalid state");

    if (hdr->packet_type >= PACKET_MAX) {
        ESP_LOGE(TAG, "Invalid packet type: %u", hdr->packet_type);
        return FORT_ERR_PROTOCOL;
    }
    fort_pkt_handler handler = state_table[sess->state][hdr->packet_type];
    if (handler == NULL) {
        ESP_LOGE(TAG,
                 "Unexpected state (" STATE_FMT_SPEC
                 ") for receiving packet with type 0x%X",
                 hdr->packet_type, STATE_FMT(sess->state));
        return FORT_ERR_WRONG_STATE;
    }
    return handler(sess, hdr, data);
}

fort_error fort_on_pkt_hello(fort_session *sess, const fort_header *hdr,
                             const void *data)
{
    sess->state = FORT_STATE_HELLO_RECEIVED;
    xEventGroupSetBits(sess->events, FORT_EVT_GATEWAY_HELLO);
    return FORT_ERR_OK;
}

fort_error fort_on_pkt_bindr(fort_session *sess, const fort_header *hdr,
                             const void *data)
{
    if (sess->gateway_bind_port == 0) {
        ESP_LOGW(TAG, "Unexpected bind confirmation from gateway");
        return FORT_ERR_OK;
    }
    if (sess->gateway_bind_port != hdr->port) {
        ESP_LOGE(TAG,
                 "Gateway bind failure: port %u (got) != port %u (expected)",
                 hdr->port, sess->gateway_bind_port);
        sess->gateway_bind_port = 0;
        xEventGroupSetBits(sess->events, FORT_EVT_GATEWAY_BINDR);
        return FORT_ERR_GATEWAY_BIND;
    }
    sess->state = FORT_STATE_BOUND;
    xEventGroupSetBits(sess->events, FORT_EVT_GATEWAY_BINDR);
    return FORT_ERR_OK;
}

fort_error fort_on_pkt_openc(fort_session *sess, const fort_header *hdr,
                             const void *data)
{
    struct sockaddr_in addr = sess->gateway_addr;
    addr.sin_port           = htons(hdr->port);

    int sock = socket(addr.sin_family, SOCK_STREAM, 0);
    if (sock == -1) {
        ESP_LOGE(TAG, "socket creation error: %s", strerror(errno));
        return FORT_ERR_SOCKET;
    }
    int err = connect(sock, (struct sockaddr *)&addr, sizeof addr);
    if (err == -1) {
        ESP_LOGE(TAG, "connect error: %s", strerror(errno));
        close(sock);
        return FORT_ERR_CONNECT;
    }

    if (xQueueSend(sess->accept_queue, &sock, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "the queue is full, cannot accept a new connection");
        close(sock);
        return FORT_ERR_QUEUE_FULL;
    }
    return FORT_ERR_OK;
}

fort_error fort_on_pkt_blank(fort_session *sess, const fort_header *hdr,
                             const void *data)
{
    size_t len = (size_t)hdr->data_length;
    if (len) {
        ESP_LOGD(TAG, "got a BLANK packet: %.*s", (int)len, (const char *)data);
    } else {
        ESP_LOGD(TAG, "got a BLANK packet with no data");
    }
    return FORT_ERR_OK;
}

fort_error fort_on_pkt_shutd(fort_session *sess, const fort_header *hdr,
                             const void *data)
{
    if (sess->state == FORT_STATE_CLOSING) {
        // We initiated a shutdown and got a response from the gateway, close
        // the service socket and return control to fort_disconnect()
        fort_do_close(sess);
        xEventGroupSetBits(sess->events, FORT_EVT_GATEWAY_SHUTD);
        return FORT_ERR_OK;
    }

    // Gateway initiated shutdown, so it's its job to close all the
    // connections, we just respond with a SHUTD packet.
    fort_header shutd = {
        .packet_type = PACKET_SHUTD, .data_length = 0, .port = 0};
    fort_error err =
        fort_send_all(sess->service_socket, &shutd, sizeof shutd, 0);
    sess->state = FORT_STATE_CLOSED;
    if (err < FORT_ERR_OK) {
        ESP_LOGE(TAG,
                 "Can't reply with SHUTD packet, "
                 "closing the socket by ourselves instead of the gateway");
        fort_do_close(sess);
        return err;
    }
    // Wait until gateway closes the socket
    fort_error ret = FORT_ERR_OK;
    char dummy;
    ssize_t rc = recv(sess->service_socket, &dummy, 1, 0);
    if (rc < 0) {
        ESP_LOGE(TAG, "recv() error while waiting for socket to close: %s",
                 strerror(errno));
        ret = FORT_ERR_RECV;
    } else if (rc > 0) {
        ESP_LOGW(TAG, "Got unexpected data while waiting for socket to close");
        ret = FORT_ERR_PROTOCOL;
    }
    fort_do_close(sess);
    return ret;
}


// The main task that processes all incoming packets and responses on them.
// Also responsible for finalizing the session by calling fort_do_close() in
// packet handling functions.
// TODO: split into two tasks: network and internal logic (state machine)
void fort_task(void *parameters)
{
    fort_session *sess = &fort_main_session;
    struct pollfd fds[20];
    int nevents = 0;
    size_t nfds = 1;
    fort_error err;

    for (;;) {
        xEventGroupWaitBits(fort_main_session.events, FORT_EVT_SERVER_HELLO,
                            pdTRUE, pdTRUE, portMAX_DELAY);
        fds[0].fd      = sess->service_socket;
        fds[0].events  = POLLIN | POLLHUP | POLLERR;
        fds[0].revents = 0;

        for (;;) {
            nevents = poll(fds, nfds, 1000);
            if (nevents < 0) {
                ESP_LOGE(TAG, "poll error: %s", strerror(errno));
            } else if (nevents == 0) {
                continue;
            }

            if (fds[0].revents & (POLLHUP | POLLERR)) {
                // Service tcp connection has been closed by gateway or an error
                // occurred
                ESP_LOGE(TAG,
                         "Unexpected service connsection close, closing the "
                         "session");
                xSemaphoreTake(sess->lock, portMAX_DELAY);
                fort_do_close(&fort_main_session);
                xSemaphoreGive(sess->lock);
                break;
            }

            if (fds[0].revents & POLLIN) {
                err = receive_packet_step(sess);
                if (err != FORT_ERR_OK) {
                    ESP_LOGW(TAG, "Error while processing a packet: %s",
                             fort_strerror(err));
                }
                if (sess->state == FORT_STATE_CLOSED) {
                    // the session just has been closed
                    break;
                }
            }
            fds[0].revents = 0;
        }
        xEventGroupClearBits(sess->events, (EventBits_t)FORT_EVT_SERVER_HELLO);
    }

    vTaskDelete(NULL);
}

// clang-format off
const fort_pkt_handler state_table[FORT_STATE_MAX][PACKET_MAX] = {
    [FORT_STATE_UNITIALIZED] = {},
    [FORT_STATE_IDLE] = {},
    [FORT_STATE_HELLO_SENT] = {
        [PACKET_HELLO] = fort_on_pkt_hello,
        [PACKET_BLANK] = fort_on_pkt_blank
    },
    [FORT_STATE_HELLO_RECEIVED] = {
        [PACKET_BINDR] = fort_on_pkt_bindr,
        [PACKET_SHUTD] = fort_on_pkt_shutd,
        [PACKET_BLANK] = fort_on_pkt_blank
    },
    [FORT_STATE_BOUND] = {
        [PACKET_OPENC] = fort_on_pkt_openc,
        [PACKET_SHUTD] = fort_on_pkt_shutd,
        [PACKET_BLANK] = fort_on_pkt_blank
    },
    [FORT_STATE_CLOSING] = {
        [PACKET_SHUTD] = fort_on_pkt_shutd,
        [PACKET_BLANK] = fort_on_pkt_blank
    },
    [FORT_STATE_CLOSED] = {},
};
// clang-format on

fort_session fort_main_session = {
    .error             = FORT_ERR_OK,
    .state             = FORT_STATE_UNITIALIZED,
    .gateway_bind_port = 0,
    .service_socket    = -1,
    .gateway_addr      = {},
    .events            = NULL,
    .accept_queue      = NULL,
    .lock              = NULL,
};

// initialized by fort_begin
fort_globals_t fort_globals = {.fort_task = NULL};