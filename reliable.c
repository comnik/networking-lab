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

#define PAYLOAD_SIZE 500

// A ringbuffer is used to buffer packets
// that are waiting for further processing.
struct ringbuf {
    uint32_t    reader;
    uint32_t    writer;
    size_t      size;
    size_t      count;
    packet_t*   buffer;
};

// Returns the number of available slots for the writer.
uint32_t buf_space (struct ringbuf* buf) {
    return buf->size - buf->count;
}

// Places a packet on the ringbuffer if space is left.
// Returns 0 if the operation succeeded, 1 otherwise.
int put_pkt (struct ringbuf* buf, packet_t* pkt) {
    if (buf->count < buf->size) {
        buf->buffer[buf->writer] = *pkt;
        buf->writer = (buf->writer + 1) % (buf->size);
        buf->count += 1;

        return 0;
    } else {
        return 1;
    }
}

// Reads the next packet from the ringbuffer, if one is available.
packet_t* read_pkt (struct ringbuf* buf) {
    if (buf->count > 0) {
        return &(buf->buffer[buf->reader]);
    } else {
        return NULL;
    }
}

// Advances the reader position.
int pop_pkt (struct ringbuf* buf) {
    packet_t* next_pkt = read_pkt(buf);

    if (next_pkt != NULL) {
        buf->reader = (buf->reader + 1) % (buf->size);
        buf->count -= 1;

        return 0;
    } else {
        return 1;
    }
}

struct reliable_state {
    rel_t*      next;   // Linked list for traversing all connections
    rel_t**     prev;

    conn_t*     c;      // The connection

    struct ringbuf* pkt_buf;
    packet_t    pkt_recieved; // Last packet recieved.

    uint32_t    next_seqno; // The first sequence number in this stream.
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
    r->next_seqno = 1;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    // Initialize the buffers
    r->pkt_buf = (struct ringbuf*) xmalloc(sizeof(struct ringbuf));
    r->pkt_buf->writer = 0;
    r->pkt_buf->reader = 0;
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
    // Transform back to host ordering.
    pkt->seqno  = ntohl(pkt->seqno);
    pkt->ackno  = ntohl(pkt->ackno);
    pkt->len    = ntohs(pkt->len);

    // Check if we're dealing with an ACK.
    if (pkt->len == 8) {
        // Advance LAR up to the highest seqno ACKed.
        packet_t* next_pkt = read_pkt(r->pkt_buf);

        while (next_pkt != NULL && ntohs(next_pkt->seqno) < pkt->ackno) {
            fprintf(stderr, "[ACK] %u\n", ntohs(next_pkt->seqno));
            pop_pkt(r->pkt_buf);
            next_pkt = read_pkt(r->pkt_buf);
        }

        // Buffer has space now, read remaining inputs.
        rel_read(r);

    } else {
        // Send ACK.
        struct ack_packet* ack = (struct ack_packet*) xmalloc(sizeof(struct ack_packet));
        ack->cksum  = 0;
        ack->ackno  = htonl(pkt->seqno + 1); // We are waiting for the next higher seqno.
        ack->len    = htons(8);
        ack->cksum  = cksum(ack, 8);

        if (r->pkt_buf->count == 0) {
            // No outgoing packets, simply go ahead and send it.
            put_pkt(r->pkt_buf, (packet_t*) ack);
            conn_sendpkt(r->c, (packet_t*) ack, 8);

            fprintf(stderr, "Sending ACK for %u\n", ntohl(ack->ackno));
        } else {
            // @TODO: Piggybacking!
            conn_sendpkt(r->c, (packet_t*) ack, 8); // Not good, ACKs need to be buffered as well
        }

        // Output payload.
        int bytes_outputted = conn_output(r->c, pkt->data, (pkt->len - 12));
        if (bytes_outputted < 0) {
            fprintf(stderr, "There was an error.\n");
        }

        r->pkt_recieved = *pkt;
    }

}

// Called once we are supposed to send some data over a connection.
void rel_read (rel_t *s) {
    void* inp_buf = xmalloc(PAYLOAD_SIZE);

    if (buf_space(s->pkt_buf) > 0) {
        // Read a single packet into the current window.
        int bytes_read = conn_input(s->c, inp_buf, PAYLOAD_SIZE);

        if (bytes_read >= 0) {
            // Construct a packet.
            packet_t* pkt = (packet_t*) xmalloc(sizeof(packet_t));
            pkt->cksum  = 0;
            pkt->seqno  = htonl(s->next_seqno);
            pkt->len    = htons(bytes_read + 12);

            // Set the payload.
            if (bytes_read > 0) {
                memcpy(pkt->data, inp_buf, bytes_read);
            }

            // Compute checksum.
            pkt->cksum = cksum(pkt, bytes_read + 12);

            // Update seqno counter.
            s->next_seqno += 1;

            if (put_pkt(s->pkt_buf, pkt) == 0) {
                // The packet is placed under LAST_FRAME_SENT, so we need to actually send it.
                fprintf(stderr, "[SEND] %u\n", ntohs(pkt->seqno));
                conn_sendpkt(s->c, pkt, pkt->len);
            }
        }
    }
}

// Called whenever output space becomes available.
void rel_output (rel_t *r) {
    size_t bytes_available = conn_bufspace(r->c);
    fprintf(stderr, "Outputting %lu bytes.", bytes_available);

    conn_output(r->c, r->pkt_recieved.data, bytes_available);
}

void rel_timer () {
    /* Retransmit any packets that need to be retransmitted */
}
