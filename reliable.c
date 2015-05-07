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


// [BUFFER]

// A ringbuffer is used to buffer packets
// that are waiting for further processing.
struct ringbuf {
    uint32_t    reader; // really the LAR pointer in our case
    uint32_t    writer; // really the LFS pointer in our case
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


// [STATE]

struct reliable_state {
    rel_t*      next; // Linked list for traversing all connections
    rel_t**     prev;

    conn_t*     c; // The connection

    struct ringbuf* pkt_buf;

    // The highest unacked seqno of the packet buffer <interval>-ago.
    // Each packet in the current buffer with seqno <= latest_seqno_snapshot has timed out.
    uint32_t    latest_seqno_snapshot;

    int         timer; // Store timer configuration.
    int         timeout; // Store timeout configuration.
    int         next_timeout; // Milliseconds until the next timeout period.

    uint32_t    next_ackno; // Next packet expected in this stream.
    uint32_t    next_seqno; // The next sequence number in this stream.

    // State flags
    int         read_error;
};
rel_t *rel_list;


// [HELPER FUNCTIONS]

// Returns a packets seqno in host-order.
// @TODO: Could be implemented as a macro to save the function call.
uint32_t get_seqno(packet_t* pkt) {
    return ntohl(pkt->seqno);
}

// Returns a packets length in host-order.
// @TODO: Could be implemented as a macro to save the function call.
uint32_t get_size(packet_t* pkt) {
    return ntohs(pkt->len);
}

// Tries to enqueue a new packet with the given payload in the current window.
// Will automatically transform the packet data to network ordering.
// If this succeeds, it will immediately send the packet.
// Returns 0 if it succeeded, 1 otherwise.
int ingest_pkt (rel_t* r, void* payload, int len) {
    // Check for buffer space early, so we don't waste work.
    if (buf_space(r->pkt_buf) > 0) {
        uint16_t pkt_size = 12;
        if (len > 0) {
            pkt_size = len + 12;
        }

        packet_t* pkt = (packet_t*) xmalloc(sizeof(packet_t));

        pkt->cksum  = 0;
        pkt->seqno  = htonl(r->next_seqno++);
        pkt->ackno  = htonl(r->next_ackno);
        pkt->len    = htons(pkt_size); // payload size + header size

        // Set payload.
        if (len > 0) {
            memcpy(pkt->data, payload, len);
        }

        // Compute checksum.
        pkt->cksum  = cksum(pkt, pkt_size); // don't use pkt->len here, it's in network order

        // The packet is placed under LAST_FRAME_SENT, so we need to actually send it.
        if (len > 0) {
            // Enqueue, guaranteed to succeed because we checked for buf_space above.
            put_pkt(r->pkt_buf, pkt);
            // fprintf(stderr, "[SEND] %u\n", get_seqno(pkt));
        }

        conn_sendpkt(r->c, pkt, pkt_size);

        return 0;
    } else {
        return 1;
    }
}

// Identifies timed-out packets for a connection
// and updates the latest_seqno_snapshot.
void resend (rel_t* r) {
    r->next_timeout -= r->timer; // Count down.

    // Check if timeouts are possible.
    if (r->next_timeout <= 0 && r->pkt_buf->count > 0) {
        packet_t* next_pkt;
        int i;

        // Iterate through all packets in the buffer.
        for (i = 0; i < r->pkt_buf->count; i++) {
            next_pkt = &(r->pkt_buf->buffer[r->pkt_buf->reader + i]);

            if (get_seqno(next_pkt) <= r->latest_seqno_snapshot) {
                fprintf(stderr, "[RE-SEND] %u\n", get_seqno(next_pkt));
                conn_sendpkt(r->c, next_pkt, get_size(next_pkt));
            }
        }

        // We need the seqno of the packet that was added last.
        // Because we already iterated through the buffer from oldest -> youngest,
        // this is already stored in next_pkt.
        r->latest_seqno_snapshot = get_seqno(next_pkt);

        r->next_timeout = r->timeout; // Reset the countdown.
    }
}


// Acknowledges the recieval of the specified packet.
void ack_pkt (rel_t* r, packet_t* pkt) {
    uint32_t ackno = htonl(++r->next_ackno);

    if ((r->pkt_buf->count == 0)) {
        // No outgoing packets, simply go ahead and send it.
        struct ack_packet* ack = (struct ack_packet*) xmalloc(sizeof(struct ack_packet));

        ack->cksum  = 0;
        ack->ackno  = ackno;
        ack->len    = htons(8);
        ack->cksum  = cksum(ack, 8);

        conn_sendpkt(r->c, (packet_t*) ack, 8);
    } else {
        // Buffer full.
        // Piggyback off of the next outgoing packet.
        packet_t* pkt_out = &(r->pkt_buf->buffer[r->pkt_buf->writer - 1]);

        pkt_out->ackno = ackno;
        pkt_out->cksum = cksum(pkt_out, get_size(pkt_out));
    }
}


// [LOGIC]

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

    // Config sanity checks.
    if (cc->window < 1) {
        fprintf(stderr, "Window must at least be 1.\n");
        exit(1);
    }

    // Initialize sender / reciever state.
    r->next_seqno = 1;
    r->next_ackno = 1;

    // Initialize timeout detection.
    r->latest_seqno_snapshot = 0;
    r->timer = cc->timer;
    r->timeout = cc->timeout;
    r->next_timeout = cc->timeout;

    // Initialize the buffers
    r->pkt_buf = (struct ringbuf*) xmalloc(sizeof(struct ringbuf));
    r->pkt_buf->writer = 0;
    r->pkt_buf->reader = 0;
    r->pkt_buf->size = cc->window;
    r->pkt_buf->buffer = (packet_t*) calloc(cc->window, sizeof(packet_t));

    // Initialize state flags
    r->read_error = 0;

    return r;
}

// Frees all allocated memory.
void rel_destroy (rel_t *r) {
    if (r->next) {
        r->next->prev = r->prev;
    }

    *r->prev = r->next;
    conn_destroy (r->c);

    // @TODO: This causes a segfault at the moment.

    // Free packet contents, buffer space
    // and finally the buffer struct itself.
    // size_t i;
    // for (i = 0; i < r->pkt_buf->size; i++) {
    //     free(r->pkt_buf->buffer[i].data);
    // }
    // free(r->pkt_buf->buffer);
    // free(r->pkt_buf);
}

// Called whenever we have recieved a packet.
void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    // Transform back to host ordering.
    pkt->ackno  = ntohl(pkt->ackno);

    // Check if we're dealing with an ACK.
    if (get_size(pkt) == 8) {
        // Advance LAR up to the highest seqno ACKed.
        packet_t* next_pkt = read_pkt(r->pkt_buf);

        while (next_pkt != NULL && get_seqno(next_pkt) < pkt->ackno) {
            // fprintf(stderr, "[ACK] %u\n", get_seqno(next_pkt));
            pop_pkt(r->pkt_buf);
            next_pkt = read_pkt(r->pkt_buf);
        }

        // Buffer has space now, read remaining inputs.
        rel_read(r);

    } else if (get_size(pkt) == 12) {
        // EOF
        fprintf(stderr, "Recieved EOF\n");

        conn_output(r->c, pkt->data, 0);

        if (r->read_error && (r->pkt_buf->count == 0)) {
            rel_destroy(r);
        }

    } else {
        // Send ACK.
        ack_pkt(r, pkt);

        // Output payload.
        int bytes_outputted = conn_output(r->c, pkt->data, (get_size(pkt) - 12));
        if (bytes_outputted < 0) {
            fprintf(stderr, "There was an error.\n");
        }
    }

}

// Called once we are supposed to send some data over a connection.
void rel_read (rel_t *s) {
    void* inp_buf = xmalloc(PAYLOAD_SIZE);

    if (buf_space(s->pkt_buf) > 0) {
        // Read a single packet into the current window.
        int bytes_read = conn_input(s->c, inp_buf, PAYLOAD_SIZE);

        s->read_error = 0;
        if (bytes_read == -1) {
            s->read_error = 1;
            fprintf(stderr, "[EOF]\n");
        } else if (bytes_read == 0) {
            return;
        }

        ingest_pkt(s, inp_buf, bytes_read);
    }
}

// Called whenever output space becomes available.
void rel_output (rel_t *r) {
    size_t bytes_available = conn_bufspace(r->c);
    fprintf(stderr, "\t -> Can now output %lu more bytes.", bytes_available);

    // @TODO
}

// Calls resend on every active connection.
void rel_timer () {
    // fprintf(stderr, "\t -> [TIMER] \n");

    if (rel_list) {
        rel_t* r = rel_list;
        while (r != NULL) {
            resend(r);
            r = r->next;
        }
    }
}
