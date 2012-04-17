#include <stdlib.h>
#include <dispatch/dispatch.h>
#include "process.h"

// spawn new process
process_process* process_spawn(node_node* node, process_process_function function) {
    // TODO
    // get free message queue
    process_id id = 0;
    message_queue* queue = node_message_queue_get(node, &id);

    // create process struct
    process_process* process = malloc(sizeof(process_process));
    process->queue = queue;
    process->pid = id;
    process->process_node = node;

    // call process function
    dispatch_async(node->concurrent_queue, ^(void) {
            function(process);
        });

    // return error pid
    return process;
}

// message sendig
message_message* process_message_send(node_node* node, process_id dest_id,
    message_message* message) {
    // check for correct dest_id
    if (dest_id >= node->process_size) {
        return NULL;
    }

    // get destination message queue
    message_queue* dest_queue = &(node->process_message_queues[dest_id]);

    // enqueue message
    message_queue_put(dest_queue, message);

    return message;
}

// process cleanup
void process_cleanup(process_process* process) {
    // free process memory
    free(process);
}
