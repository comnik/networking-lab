#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"


// A ringbuffer is used to buffer packets
// that are waiting for further processing.
struct ringbuf {
    uint32_t    reader;
    uint32_t    writer;
    size_t      size;
    packet_t*   buffer;
};

// Places a packet on the ringbuffer if space is left.
// Returns 0 if the operation succeeded, 1 otherwise.
int put_pkt(struct ringbuf* buf, packet_t* pkt) {
    if (labs(buf->writer - buf->reader) <= buf->size) {
        buf->buffer[buf->writer] = *pkt;
        buf->writer = (buf->writer + 1) % (buf->size);

        return 0;
    } else {
        return 1;
    }
}

// Pops the next packet from the ringbuffer, if one is available.
// Returns 0 if the operation suceeded, 1 otherwise.
int pop_pkt(struct ringbuf* buf, packet_t* pkt_out) {
    if (buf->writer < buf->reader) {
        *pkt_out = buf->buffer[buf->reader];
        buf->reader = (buf->reader + 1) % (buf->size);

        return 0;
    } else {
        // No packages left.
        return 1;
    }
}

struct reliable_state {
    rel_t*      next;   // Linked list for traversing all connections
    rel_t**     prev;

    conn_t*     c;      // The connection

    struct ringbuf* pkt_buf;
};
rel_t *rel_list;

// Creates a new reliable protocol session,
// returns NULL on failure. ss is always NULL. */
rel_t* rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc) {
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    // Initialize the buffers
    r->pkt_buf = (struct ringbuf*) xmalloc(sizeof(struct ringbuf));
    r->pkt_buf->size = cc->window;
    r->pkt_buf->buffer = (packet_t*) calloc(cc->window, sizeof(packet_t));

    return r;
}

// Frees all allocated memory.
void rel_destroy (rel_t *r) {
    if (r->next) {
        r->next->prev = r->prev;
    }

    *r->prev = r->next;
    conn_destroy (r->c);

    // Free packet contents, buffer space
    // and finally the buffer struct itself.
    size_t i;
    for (i = 0; i < r->pkt_buf->size; i++) {
        free(r->pkt_buf->buffer[i].data);
    }
    free(r->pkt_buf->buffer);
    free(r->pkt_buf);
}

// Called whenever we have recieved a packet.
void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    printf("Recieved something!\n");
}

// Called once we are supposed to send some data over a connection.
void rel_read (rel_t *s) {
    int bytes_read;
    void* inp_buf = xmalloc(500);

    printf("Reading from input...\n");

    // Read as much packets as possible into the current window.
    while ((bytes_read = conn_input(s->c, inp_buf, 500)) > 0) {
        printf("\t-> %i bytes", bytes_read);

        // Construct a packet.
        packet_t* pkt = (packet_t*) xmalloc(sizeof(packet_t));
        pkt->cksum = cksum(inp_buf, bytes_read); // compute 16-bit checksum
        pkt->len = bytes_read;
        memcpy(inp_buf, pkt->data, bytes_read);

        if (put_pkt(s->pkt_buf, pkt)) {
            // The packet is placed under LAST_FRAME_SENT.
            // So lets send it..
            printf("[SEND]\n");
            conn_sendpkt(s->c, pkt, pkt->len);
        } else {
            // The window is full.
            break;
        }
    }
}

void rel_output (rel_t *r) {

}

void rel_timer () {
    /* Retransmit any packets that need to be retransmitted */
}
