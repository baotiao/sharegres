#include "copy_handler.h"
#include "../protocol/message.h"
#include "../log.h"

#include <string.h>
#include <arpa/inet.h>

/*
 * Forward a CopyInResponse or CopyOutResponse from the backend to the client.
 * The raw_msg includes the type byte.
 */
void copy_forward_response(ClientSession *session, const uint8_t *raw_msg,
                           size_t msg_len)
{
    session_queue_send(session, raw_msg, msg_len);
}

/*
 * Handle COPY IN client messages (CopyData 'd', CopyDone 'c', CopyFail 'f').
 * Forward them to the backend connection.
 */
int copy_handle_client_message(ClientSession *session, uint8_t msg_type,
                               const uint8_t *payload, size_t payload_len)
{
    PGconn *conn = session->backend_conn;
    if (!conn) {
        log_error("copy: no backend connection");
        return -1;
    }

    switch (msg_type) {
    case MSG_COPY_DATA: {
        /* Forward CopyData payload to backend via PQputCopyData */
        int rc = PQputCopyData(conn, (const char *)payload, (int)payload_len);
        if (rc < 0) {
            log_error("copy: PQputCopyData failed: %s", PQerrorMessage(conn));
            return -1;
        }
        return 0;
    }

    case MSG_COPY_DONE: {
        /* Signal end of COPY data */
        int rc = PQputCopyEnd(conn, NULL);
        if (rc < 0) {
            log_error("copy: PQputCopyEnd failed: %s", PQerrorMessage(conn));
            return -1;
        }
        PQflush(conn);
        log_info("copy: CopyDone sent to backend");
        return 1; /* COPY finished, wait for backend CommandComplete */
    }

    case MSG_COPY_FAIL: {
        /* Forward error to backend */
        const char *errmsg = payload_len > 0 ? (const char *)payload : "client cancel";
        int rc = PQputCopyEnd(conn, errmsg);
        if (rc < 0) {
            log_error("copy: PQputCopyEnd(fail) failed: %s", PQerrorMessage(conn));
            return -1;
        }
        PQflush(conn);
        log_info("copy: CopyFail sent to backend: %s", errmsg);
        return 1;
    }

    default:
        log_warn("copy: unexpected message type 0x%02x during COPY", msg_type);
        return -1;
    }
}
