// libactor
//
// Implementation of an erlang style actor model using libdispatch
// Copyright (C) 2012  Patrik Gebhardt
// Contact: patrik.gebhardt@rub.de
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include "../include/actor.h"

// message send process
actor_error_t actor_distributer_message_send(actor_process_t self, int sock) {
    // create header
    actor_distributer_header_s header;

    // error
    actor_error_t error = ACTOR_SUCCESS;

    // message pointer
    actor_message_t message = NULL;

    // send loop
    while (true) {
        // get message
        actor_message_t message = NULL;
        error = actor_receive(self, &message, 10.0);

        // check error
        if (error != ACTOR_SUCCESS) {
            return error;
        }

        // on dedicated message close connection
        if ((message->destination_nid == self->nid) &&
            (message->destination_pid == self->pid)) {
            // cleanup
            actor_message_release(&message);

            break;
        }

        // create header
        header.dest_id = message->destination_pid;
        header.message_size = message->size;
        header.type = message->type;

        // send header
        if (send(sock, &header, sizeof(actor_distributer_header_s), 0) <= 0) {
            return ACTOR_ERROR_NETWORK;
        }

        // send message
        if (send(sock, message->data, message->size, 0) <= 0) {
            return ACTOR_ERROR_NETWORK;
        }

        // release message
        actor_message_release(&message);
    }

    return ACTOR_SUCCESS;
}

// message receive process
actor_error_t actor_distributer_message_receive(actor_process_t self, int sock) {
    int bytes_received;
    actor_distributer_header_s header;

    // get messages
    while (true) {
        // receive header
        bytes_received = recv(sock, &header,
            sizeof(actor_distributer_header_s), 0);

        // check for closed connection
        if (bytes_received == 0) {
            return ACTOR_ERROR_NETWORK;
        }
        // check for message error
        else if (bytes_received == -1) {
            continue;
        }

        // check correct header
        if (bytes_received != sizeof(actor_distributer_header_s)) {
            continue;
        }

        // create message buffer
        char* data = malloc(header.message_size);

        // total received data
        actor_size_t total_received = 0;

        // receive message
        while (true) {
            // get chunk
            bytes_received = recv(sock, &data[total_received],
                header.message_size - total_received, 0);

            // check for closed connection
            if (bytes_received == 0) {
                // cleanup
                free(data);

                return ACTOR_ERROR_NETWORK;
            }
            // check for message error
            else if (bytes_received == -1) {
                // cleanup
                free(data);
                data = NULL;

                break;
            }

            // increase total size
            total_received += bytes_received;

            // check length
            if (total_received >= header.message_size) {
                break;
            }
        }

        // send message
        actor_send(self, self->nid, header.dest_id,
            header.type, data, header.message_size);

        // free message buffer
        free(data);
    }

    return ACTOR_SUCCESS;
}

// connection supervisor
actor_error_t actor_distributer_connection_supervisor(actor_process_t self,
    actor_node_id_t remote_node, int sock) {
    // start send process
    actor_process_id_t sender = ACTOR_INVALID_ID;

    // loop
    while (true) {
        // receive error message
        actor_message_t message = NULL;
        if (actor_receive(self, &message, 10.0) != ACTOR_SUCCESS) {
            continue;
        }

        // check message
        if (message->type != ACTOR_TYPE_ERROR_MESSAGE) {
            // cleanup
            actor_message_release(&message);

            continue;
        }

        // cast to error message
        actor_process_error_message_t error_message =
            (actor_process_error_message_t)message->data;

        // check error message
        if (error_message->error == ACTOR_ERROR_TIMEOUT) {
            // restart sender
            actor_error_t error = actor_spawn(self->node, &sender,
                ^actor_error_t(actor_process_t s) {
                    // set self as supervisor
                    actor_process_link(s, self->nid, self->pid);

                    // start send process
                    return actor_distributer_message_send(s, sock);
                });

            // set new connector
            self->node->remote_nodes[remote_node] = sender;
        }
        else {
            // release message
            actor_message_release(&message);

            break;
        }

        // release message
        actor_message_release(&message);
    }

    // close connection
    shutdown(sock, 2);

    // try close send process
    actor_distributer_disconnect_from_node(self->node, remote_node);

    // invalid connection
    self->node->remote_nodes[remote_node] = ACTOR_INVALID_ID;

    return ACTOR_SUCCESS;
}

actor_error_t actor_distributer_start_connectors(actor_node_t node,
    actor_node_id_t remote_node, int sock) {
    // check input
    if (node == NULL) {
        return ACTOR_ERROR_INVALUE;
    }

    // error
    actor_error_t error = ACTOR_SUCCESS;

    // start connection supervisor
    actor_process_id_t supervisor = ACTOR_INVALID_ID;
    error = actor_spawn(node, &supervisor,
        ^actor_error_t(actor_process_t self) {
            return actor_distributer_connection_supervisor(self, remote_node, sock);
        });

    // check success
    if (error != ACTOR_SUCCESS) {
        return error;
    }

    // start receive process
    actor_process_id_t receiver = ACTOR_INVALID_ID;
    error = actor_spawn(node, &receiver,
        ^actor_error_t(actor_process_t self) {
            // set self as supervisor
            actor_process_link(self, self->nid, supervisor);

            // start receive process
            return actor_distributer_message_receive(self, sock);
        });

    // check success
    if (error != ACTOR_SUCCESS) {
        return error;
    }

    // start send process
    actor_process_id_t sender = ACTOR_INVALID_ID;
    error = actor_spawn(node, &sender,
        ^actor_error_t(actor_process_t self) {
            // set self as supervisor
            actor_process_link(self, self->nid, supervisor);

            // start send process
            return actor_distributer_message_send(self, sock);
        });

    // check success
    if (error != ACTOR_SUCCESS) {
        return error;
    }

    // init sender as connector
    node->remote_nodes[remote_node] = sender;

    return ACTOR_SUCCESS;
}

// connect to node
actor_error_t actor_distributer_connect_to_node(actor_node_t node, actor_node_id_t* nid,
    const char* host_name, unsigned int port, const char* key) {
    // check valid node
    if (node == NULL) {
        return ACTOR_ERROR_INVALUE;
    }

    // check key length
    if (strlen(key) > ACTOR_DISTRIBUTER_KEYLENGTH) {
        return ACTOR_ERROR_INVALUE;
    }

    // init node id pointer
    if (nid != NULL) {
        *nid = ACTOR_INVALID_ID;
    }

    // create client socket
    int bytes_received;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // check success
    if (sock == -1) {
        return ACTOR_ERROR_NETWORK;
    }

    // set recv timeout to 10 sec
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (struct timeval*)&tv, sizeof(struct timeval));

    // get host address
    struct hostent* host = gethostbyname(host_name);

    // create server address struct
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)host->h_addr);
    bzero(&(server_addr.sin_zero),8);

    // connect
    if (connect(sock, (struct sockaddr *)&server_addr,
            sizeof(struct sockaddr)) == -1) {
        return ACTOR_ERROR_NETWORK;
    }

    // copy key to static sized buffer
    char buffer[ACTOR_DISTRIBUTER_KEYLENGTH + 1];
    strcpy(buffer, key);

    // send key
    if (send(sock, &buffer, ACTOR_DISTRIBUTER_KEYLENGTH + 1, 0) <= 0) {
        // close connection
        close(sock);

        return ACTOR_ERROR_NETWORK;
    }

    // send node id
    if (send(sock, &node->id, sizeof(actor_node_id_t), 0) <= 0) {
        // close
        close(sock);

        return ACTOR_ERROR_NETWORK;
    }

    // get node id
    actor_node_id_t node_id;
    bytes_received = recv(sock, &node_id, sizeof(actor_node_id_t), 0);

    // check success
    if (bytes_received != sizeof(actor_node_id_t)) {
        // close connection
        close(sock);

        return ACTOR_ERROR_NETWORK;
    }

    if ((node_id < 0) || (node_id > ACTOR_NODE_MAX_REMOTE_NODES) ||
        (node->remote_nodes[node_id] != ACTOR_INVALID_ID) ||
        (node_id == node->id)) {
        // close connection
        close(sock);

        return ACTOR_ERROR_NETWORK;
    }

    // start connectors
    actor_error_t error = actor_distributer_start_connectors(node, node_id, sock);

    // check success
    if (error != ACTOR_SUCCESS) {
        // close connection
        close(sock);

        return error;
    }

    // set node id
    if (nid != NULL) {
        *nid = node_id;
    }

    return ACTOR_SUCCESS;
}

// listen incomming connections
actor_error_t actor_distributer_listen(actor_node_t node, actor_node_id_t* nid,
    unsigned int port, const char* key) {
    // check valid node
    if (node == NULL) {
        return ACTOR_ERROR_INVALUE;
    }

    // check key length
    if (strlen(key) > ACTOR_DISTRIBUTER_KEYLENGTH) {
        return ACTOR_ERROR_INVALUE;
    }

    // init node id pointer
    if (nid != NULL) {
        *nid = ACTOR_INVALID_ID;
    }

    // create server socket
    int bytes_received;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // check success
    if (sock == -1) {
        return ACTOR_ERROR_NETWORK;
    }

    // set socket opts
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    // create server address struct
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    bzero(&(server_addr.sin_zero),8);

    // bind socket to address
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        return ACTOR_ERROR_NETWORK;
    }

    // start listening
    if (listen(sock, 5)) {
        return ACTOR_ERROR_NETWORK;
    }

    // accept incomming connections
    unsigned int sin_size = sizeof(struct sockaddr_in);
    struct sockaddr_in client_addr;
    int connected = accept(sock, (struct sockaddr *)&client_addr, &sin_size);

    // close socket
    close(sock);

    // check success
    if (connected == -1) {
        return ACTOR_ERROR_NETWORK;
    }

    // set recv timeout to 10 sec
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, (struct timeval*)&tv,
        sizeof(struct timeval));

    // get key
    char remote_key[ACTOR_DISTRIBUTER_KEYLENGTH + 1];
    if (recv(connected, &remote_key, sizeof(char) * (ACTOR_DISTRIBUTER_KEYLENGTH + 1),
        0) != ACTOR_DISTRIBUTER_KEYLENGTH + 1) {
        // close connection
        close(connected);

        return ACTOR_ERROR_NETWORK;
    }

    // check key
    if (strcmp(key, remote_key) != 0) {
        // close connection
        close(connected);

        return ACTOR_ERROR_NETWORK;
    }

    // send node id
    if (send(connected, &node->id, sizeof(actor_node_id_t), 0) <= 0) {
        // close connection
        close(connected);

        return ACTOR_ERROR_NETWORK;
    }

    // get node id
    actor_node_id_t node_id;
    bytes_received = recv(connected, &node_id, sizeof(actor_node_id_t), 0);

    // check success
    if (bytes_received != sizeof(actor_node_id_t)) {
        // close connection
        close(connected);

        return ACTOR_ERROR_NETWORK;
    }

    if ((node_id < 0) || (node_id >= ACTOR_NODE_MAX_REMOTE_NODES) ||
        (node->remote_nodes[node_id] != ACTOR_INVALID_ID) ||
        (node_id == node->id)){
        // close connection
        close(connected);

        return ACTOR_ERROR_NETWORK;
    }

    // start connectors
    actor_error_t error = actor_distributer_start_connectors(node, node_id, connected);

    // check success
    if (error != ACTOR_SUCCESS) {
        // close connection
        close(connected);

        return error;
    }

    // set node id
    if (nid != NULL) {
        *nid = node_id;
    }

    return ACTOR_SUCCESS;
}

// disconnect from node
actor_error_t actor_distributer_disconnect_from_node(actor_node_t node, actor_node_id_t nid) {
    // check input
    if ((node == NULL) || (nid < 0) || (nid >= ACTOR_NODE_MAX_REMOTE_NODES)) {
        return ACTOR_ERROR_INVALUE;
    }

    // check connection state
    if (node->remote_nodes[nid] == ACTOR_INVALID_ID) {
        return ACTOR_ERROR_NETWORK;
    }

    // send disconnect message
    return actor_node_send_message(node, node->id, node->remote_nodes[nid],
        ACTOR_TYPE_CHAR, "STOP", 5);
}
