#include <stdlib.h>
#include <unistd.h>
#include "message.h"

// create new queue
message_queue* message_queue_create(dispatch_queue_t dispatch_queue) {
    // create new message struct
    message_queue* queue = malloc(sizeof(message_queue));

    // init parameter
    queue->dispatch_queue = dispatch_queue;
    queue->first = NULL;
    queue->last = NULL;

    return queue;
}

// cleanup
void message_queue_cleanup(message_queue* queue) {
    // get first message
    message_message* message = queue->first;

    // pointer to next message
    message_message* next = NULL;

    // free all messages
    while (message != NULL) {
        // get next message
        next = (message_message*)message->next;

        // free message
        free(message);

        // continue to next message
        message = next;
    }

    // free queue
    free(queue);
}

// add new message to queue
void message_queue_put(message_queue* queue, message_message* message) {
    // check if first message is NULL
    if (queue->first == NULL) {
        // set new message as first and last
        queue->first = message;
        queue->last = message;
        message->next = NULL;
    }
    else {
        // set new message as last
        queue->last->next = (struct message_message*)message;
        queue->last = message;
    }
}

// get message from queue
message_message* message_queue_get(message_queue* queue) {
    // check for message
    while (queue->first == NULL) {
        usleep(100);
    }

    // get message
    message_message* message = queue-> first;

    // set new first message to next
    queue->first = (message_message*)message->next;

    // if first is NULL set last to NULL
    if (queue->first == NULL) {
        queue->last = NULL;
    }

    // set next element of message to NULL
    message->next = NULL;

    return message;
}
