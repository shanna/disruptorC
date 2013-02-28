/*
 *    Copyright (C) 2012 by Jules Colding <jcolding@gmail.com>.
 *
 *    All Rights Reserved.
 */

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1) Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *
 * 3) The name of the author may not be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL * THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>

#ifdef HAVE_CONFIG_H
    #include "ac_config.h"
#endif

#define YIELD() sched_yield()
#include "src/disruptor.h"

#define STOP UINT64_MAX
#define ENTRIES_TO_GENERATE (400)
#define ENTRY_BUFFER_SIZE (16)
#define MAX_ENTRY_PROCESSORS (2)

DEFINE_ENTRY_TYPE(uint_fast64_t, entry_t);
DEFINE_RING_BUFFER_TYPE(MAX_ENTRY_PROCESSORS, ENTRY_BUFFER_SIZE, entry_t, ring_buffer_t);
DEFINE_RING_BUFFER_MALLOC(ring_buffer_t);
DEFINE_RING_BUFFER_INIT(ENTRY_BUFFER_SIZE, ring_buffer_t);
DEFINE_RING_BUFFER_SHOW_ENTRY_FUNCTION(entry_t, ring_buffer_t);
DEFINE_RING_BUFFER_ACQUIRE_ENTRY_FUNCTION(entry_t, ring_buffer_t);
DEFINE_ENTRY_PROCESSOR_BARRIER_REGISTER_FUNCTION(ring_buffer_t);
DEFINE_ENTRY_PROCESSOR_BARRIER_UNREGISTER_FUNCTION(ring_buffer_t);
DEFINE_ENTRY_PROCESSOR_BARRIER_WAITFOR_BLOCKING_FUNCTION(ring_buffer_t);
DEFINE_ENTRY_PROCESSOR_BARRIER_RELEASEENTRY_FUNCTION(ring_buffer_t);
DEFINE_ENTRY_PUBLISHERPORT_NEXTENTRY_BLOCKING_FUNCTION(ring_buffer_t);
DEFINE_ENTRY_PUBLISHERPORT_COMMITENTRY_BLOCKING_FUNCTION(ring_buffer_t);

ring_buffer_t ring_buffer;

static int
create_thread(pthread_t * const thread_id,
              void *thread_arg,
              void *(*thread_func)(void *))
{
        int retv = 0;
        pthread_attr_t thread_attr;

        if (pthread_attr_init(&thread_attr))
                return 0;

        if (pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE))
                goto err;

        if (pthread_create(thread_id, &thread_attr, thread_func, thread_arg))
                goto err;

        retv = 1;
err:
        pthread_attr_destroy(&thread_attr);

        return retv;
}

static void*
entry_publisher_thread(void *arg)
{
        ring_buffer_t *buffer = (ring_buffer_t*)arg;
        cursor_t cursor;
        entry_t *entry;
        uint64_t reps = ENTRIES_TO_GENERATE;

        do {
                publisher_port_next_entry_blocking(buffer, &cursor);
                entry = ring_buffer_acquire_entry(buffer, &cursor);
                entry->content = cursor.sequence;
                publisher_port_commit_entry_blocking(buffer, &cursor);
        } while (--reps);

        publisher_port_next_entry_blocking(buffer, &cursor);
        entry = ring_buffer_acquire_entry(buffer, &cursor);
        entry->content = STOP;
        publisher_port_commit_entry_blocking(buffer, &cursor);
        printf("Publisher done\n");

        return NULL;
}

static void*
entry_processor_thread(void *arg)
{
        cursor_t n;
        ring_buffer_t *buffer = (ring_buffer_t*)arg;
        cursor_t cursor;
        cursor_t cursor_upper_limit;
        count_t reg_number;
        const entry_t *entry;

        // register and setup entry processor
        cursor.sequence = entry_processor_barrier_register(buffer, &reg_number);
        cursor_upper_limit.sequence = cursor.sequence;

        do {
                entry_processor_barrier_wait_for_blocking(buffer, &cursor_upper_limit);
                for (n.sequence = cursor.sequence; n.sequence <= cursor_upper_limit.sequence; ++n.sequence) { // batching
                        entry = ring_buffer_show_entry(buffer, &n);
                        if (STOP == entry->content) {
                                printf("Entry processor exiting normally\n");
                                goto out;
                        }

                        if (entry->content != n.sequence) {
                                printf("Entry processor - ERROR\n");
                                goto out;
                        }
                }
                entry_processor_barrier_release_entry(buffer, &reg_number, &cursor_upper_limit);

                ++cursor_upper_limit.sequence;
                cursor.sequence = cursor_upper_limit.sequence;
        } while (1);
out:
        entry_processor_barrier_unregister(buffer, &reg_number);
        printf("Entry processor done\n");

        return NULL;
}

int
main(int argc, char *argv[])
{
        pthread_t p_1; // entry publisher
        pthread_t p_2;
        pthread_t p_3;
        pthread_t c_1; // entry processor
        pthread_t c_2;
        ring_buffer_t *ring_buffer_heap;

        ring_buffer_heap = ring_buffer_malloc();
        if (!ring_buffer_heap) {
                printf("Malloc ring buffer - ERROR\n");
                return EXIT_FAILURE;
        }
        ring_buffer_init(ring_buffer_heap);
        ring_buffer_init(&ring_buffer);

        //
        // first as a global variable
        //
        create_thread(&c_1, &ring_buffer, entry_processor_thread);
        create_thread(&c_2, &ring_buffer, entry_processor_thread);
        sleep(1);
        create_thread(&p_1, &ring_buffer, entry_publisher_thread);
        create_thread(&p_2, &ring_buffer, entry_publisher_thread);
        create_thread(&p_3, &ring_buffer, entry_publisher_thread);

        // join entry publishers
        pthread_join(p_1, NULL);
        pthread_join(p_2, NULL);
        pthread_join(p_3, NULL);

        // join entry processors
        pthread_join(c_1, NULL);
        pthread_join(c_2, NULL);
        printf("As-Global-Variable test done\n\n");

        //
        // Now as allocated on the heap
        //
        create_thread(&c_1, ring_buffer_heap, entry_processor_thread);
        create_thread(&c_2, ring_buffer_heap, entry_processor_thread);
        sleep(1);
        create_thread(&p_1, ring_buffer_heap, entry_publisher_thread);
        create_thread(&p_2, ring_buffer_heap, entry_publisher_thread);
        create_thread(&p_3, ring_buffer_heap, entry_publisher_thread);

        // join entry publishers
        pthread_join(p_1, NULL);
        pthread_join(p_2, NULL);
        pthread_join(p_3, NULL);

        // join entry processors
        pthread_join(c_1, NULL);
        pthread_join(c_2, NULL);
        free(ring_buffer_heap);
        printf("On-The-Heap test done\n");

        return EXIT_SUCCESS;
}
