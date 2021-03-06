#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "constants.h"
#include "stats.h"
#include "structs.h"
#include "player.h"
#include "colors.h"
#include "server.h"
#include "memory.h"
#include "game.h"
#include "game_logic.h"

player_t *g_player_list;
pthread_mutex_t g_player_list_mutex;
game_t *g_game_list;
pthread_mutex_t g_game_list_mutex;
pthread_t thread_id;

// Struct to be able to set timeout of socket.
struct timeval timeout;

/// Send the message to entered socket and write the message to statistics.
/// \param socket                   Socket where the message is sent.
/// \param message                  The Message.
/// \param is_broadcast_message     Tells, if it is single message or broadcast the same messages to multiple clients.
void svr_send(int socket, char *message, int is_broadcast_message) {
    if (!is_broadcast_message)
        printf(ANSI_COLOR_BLUE "--->>>\t\t\t %s" ANSI_COLOR_RESET, message);

    bytes_sent += send(socket, message, strlen(message) * sizeof(char), 0);
    messages_sent++;
}

/// Check if ID is already in list of players or game instances.
/// \param id
/// \return
int _svr_find_id(char *id) {
    if (!id)
        return -1;

    // Check players.
    pthread_mutex_lock(&g_player_list_mutex);

    if (g_player_list != NULL) {
        player_t *player_ptr = g_player_list;

        do {
            if (strcmp(player_ptr->id, id) == 0) {
                pthread_mutex_unlock(&g_player_list_mutex);
                return 1;
            }

            player_ptr = player_ptr->next;
        } while (player_ptr != NULL);

    }

    pthread_mutex_unlock(&g_player_list_mutex);

    // Check games.
    pthread_mutex_lock(&g_game_list_mutex);

    if (g_game_list != NULL) {
        game_t *game_ptr = g_game_list;

        do {
            if (strcmp(game_ptr->id, id) == 0) {
                pthread_mutex_unlock(&g_game_list_mutex);
                return 1;
            }

            game_ptr = game_ptr->next;
        } while (game_ptr != NULL);
    }

    pthread_mutex_unlock(&g_game_list_mutex);

    return 0;
}

/// Generates unique ID for players and game instances.
/// \return
char *svr_generate_id() {
    char *id = memory_malloc(sizeof(char) * (19 + 1), 0);

    do {
        sprintf(id, "%d", rand());
    } while (_svr_find_id(id) != 0);

    return id;

}

/// Send a text messsage to all players.
/// \param message  Message to send.
void svr_broadcast(char *message) {
    if (!message)
        return;

    printf(ANSI_COLOR_BLUE "--->>> (BC)\t\t %s" ANSI_COLOR_RESET, message);

    pthread_mutex_lock(&g_player_list_mutex);
    if (g_player_list != NULL) {
        player_t *player_ptr = g_player_list;

        do {
            if (player_ptr->is_disconnected != 1)
                svr_send(player_ptr->socket, message, 1);

            player_ptr = player_ptr->next;
        } while (player_ptr != NULL);

    }
    pthread_mutex_unlock(&g_player_list_mutex);

    memory_free(message, 0);
}

/// The main data stream to each player.
/// \param arg      Pointer to newly added player.
/// \return         Status code.
void *_svr_serve_receiving(void *arg) {
    player_t *player_ptr = (player_t *) arg;
    int client_sock = player_ptr->socket;
    char *id = player_ptr->id;
    int read_size;
    int timeout_lost_conn = 0;
    int timeout_unsuccessful = 0;
    char cbuf[1024];
    char *message = NULL;

    while (timeout_lost_conn <= TIMEOUT_LOST_CONN) {
        if ((read_size = (int) recv(client_sock, cbuf, 1024 * sizeof(char), 0)) != 0) { // 0 = closed connection, -1 = unsuccessful, >0 = length of the received message.
            if (read_size == -1) { // Unsuccessful (some error occurred).
                player_ptr->is_disconnected = 1;

                timeout_unsuccessful++;

                if (timeout_unsuccessful > TIMEOUT_UNSUCCESSFUL) {
                    // Send a message back to client.
                    message = memory_malloc(sizeof(char) * 256, 0);
                    sprintf(message, "%s;kick_player\n", player_ptr->id); // Token message.
                    svr_send(player_ptr->socket, message, 0);
                    memory_free(message, 0);

                    break;
                }

            } else { // Successful.
                player_ptr->is_disconnected = 0;
                bytes_received += read_size;
                messages_received++;

                printf(ANSI_COLOR_CYAN "<<<---\t\t\t %s" ANSI_COLOR_RESET, cbuf);
                _svr_process_request(cbuf);
                memset(cbuf, 0, 1024 * sizeof(char));

                timeout_unsuccessful = 0;
            }

        } else { // Lost Connection.
            if (!player_ptr->socket) // If the player has no communication anymore. The player has disconnected correctly.
                break;

            timeout_lost_conn++;
            player_ptr->is_disconnected = 1;

            sleep(1);

            if (player_ptr->socket) // Update socket.
                client_sock = player_ptr->socket;
        }
    }

    // If the connection is lost.
    player_ptr = player_find(id);
    if (player_ptr) {
        player_ptr->is_disconnected = 1; // Means, do not bother with updating client. Client is already closed or do not have connection.

        if (player_ptr->game) {
            player_disconnect_from_game(player_ptr, player_ptr->game);
        }

        player_remove(player_ptr);
    }

    return 0;
}

/// Obtaining data from client about player creation. Create a player.
/// \param arg      Client socket.
/// \return         NULL.
void *_svr_connection_handler(void *arg) {
    char msg[64];
    char *id = NULL;
    char *tokens = NULL;
    player_t *player = NULL;
    char *log_message = NULL;
    char *nickname = NULL;
    char *message = NULL;
    int is_reconnecting = 0; // Check if user is connecting first time or he is reconnecting.

    // Fill the msg var with zeros.
    memset(msg, 0, strlen(msg));

    // Count stats.
    bytes_received += recv(((remote_connection_t *) arg)->client_socket, msg, sizeof(char) * 64, 0);
    messages_received++;

    // Set socket params (receive timeout).
    setsockopt(((remote_connection_t *) arg)->client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout));

    id = strtok(msg, ";"); // Expecting message like "1;nickname;John;".
    if (id) {
        tokens = strtok(NULL, ";");
    } else {
        messages_bad++;
    }

    // Client is trying to reconnect.
    if (
            (tokens != NULL && strcmp(tokens, "_player_reconnect") == 0)
            || (tokens != NULL && (player = player_find_unknown_reconnect(((remote_connection_t *) arg)->client_address)))
            ) { // Client is trying to reconnect.
        is_reconnecting = 1;

        if (!player)
            player = player_find(id);
        if (player)
            player_change_socket(player, ((remote_connection_t *) arg)->client_socket);

    } else {
        player = NULL;
    }

    // Client is trying to connect to server...
    if (tokens != NULL && is_reconnecting && player != NULL) { // Client is trying to reconnect.
        player->is_disconnected = 0; // Reset, client is back.

        // Send a message back to client.
        message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(message, "%s;_player_id_reconnected\n", player->id); // Token message.
        svr_send(player->socket, message, 0);
        memory_free(message, 0);

    } else if ((tokens != NULL && is_reconnecting) || (tokens != NULL && strcmp(tokens, "_player_nickname") == 0)) { // Client is firstly connecting to the server.
        is_reconnecting = 0;

        nickname = strtok(NULL, ";");

        if (!nickname) {
            sprintf(nickname, "Player"); // Default player name.
        }

        // Create player.
        player = player_create((remote_connection_t *) arg, nickname);

        // Send a message back to client.
        message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(message, "%s;_player_id\n", player->id); // Token message.
        svr_send(player->socket, message, 0);
        memory_free(message, 0);

        // Add player to the list.
        player_add(player);

    } else {
        // Log.
        log_message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(log_message, "\t> Player could not be added!\n");
        write_log(log_message);
        memory_free(log_message, 0);

        messages_bad++;

        // Free.
        memory_free(((remote_connection_t *) arg)->client_address, 0);
        memory_free(arg, 0);

        close(((remote_connection_t *) arg)->client_socket);

        return NULL;
    }

    // Create a new thread to solve player data sending separately.
    thread_id = 0;
    if (pthread_create(&thread_id, NULL, _svr_serve_receiving, (void *) player)) {
        // Unsuccessful thread start branch.
        // Log.
        log_message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(log_message, "\t> Fatal ERROR during creating a new thread!\n");
        write_log(log_message);
        memory_free(log_message, 0);

        // Send a message back to client.
        message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(message, "%s;player_crash\n", player->id); // Token message.
        svr_send(player->socket, message, 0);
        memory_free(message, 0);

        // Remove player because of unsuccessful thread start.
        player_remove(player);

        // Free.
        memory_free(((remote_connection_t *) arg)->client_address, 0);
        memory_free(arg, 0);

        close(((remote_connection_t *) arg)->client_socket);

        return NULL;
    }

    // Log.
    log_message = memory_malloc(sizeof(char) * 256, 0);
    if (is_reconnecting)
        sprintf(log_message, "\t> Player %s (ID: %s) reconnected!\n", player->nickname, player->id);
    else
        sprintf(log_message, "\t> Player %s (ID: %s) connected!\n", player->nickname, player->id);
    write_log(log_message);
    memory_free(log_message, 0);

    // Free.
    memory_free(((remote_connection_t *) arg)->client_address, 0);
    memory_free(arg, 0);

    return NULL;
}

/// Create a new socket. Listening to client connection.
/// \param arg      Port number.
void *_svr_serve_connection(void *arg) {
    int server_socket,
            client_socket;
    int return_value;
    int port = *(int *) arg;
    int flag = 1;
    char *log_message = NULL;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    socklen_t remote_addr_len;
    remote_connection_t *arguments = NULL;

    // Set timeout to 60sec.
    timeout.tv_sec = 60;
    timeout.tv_usec = 0;

    // Create a new server socket.
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket <= 0)
        return NULL;

    // Set socket params.
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &flag, sizeof(int));

    // Sets all values to zero.
    memset(&local_addr, 0, sizeof(struct sockaddr_in));

    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((u_short) port);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind socket to a address (address of the current host and port on which the server will run).
    return_value = bind(server_socket, (struct sockaddr *) &local_addr, sizeof(struct sockaddr_in));
    if (return_value == 0) {
        printf("\t> Bind: OK!\n");
    } else {
        printf("\t> Bind: ERROR!\n");
        exit(1);
    }

    // Allow to listen on the socket.
    return_value = listen(server_socket, 5); // 5 recommended.
    if (return_value == 0) {
        printf("\t> Listen: OK!\n");
    } else {
        printf("\t> Listen: ERROR!\n");
        exit(1);
    }

    for (;;) {
        // Block the process until a client connect to the server.
        // Returns a new file descriptor, and all communication on this connection should be done using the new file descriptor.
        client_socket = accept(server_socket, (struct sockaddr *) &remote_addr, &remote_addr_len);

        if (client_socket > 0) {
            // Create struct with arguments to be able to pass more than 1 parameter to connection handler.
            arguments = memory_malloc(sizeof(remote_connection_t), 0);
            arguments->client_address = memory_malloc(sizeof(char) * ((remote_connection_t *) arg)->client_address_len + 4, 0);
            inet_ntop(AF_INET, &remote_addr.sin_addr, arguments->client_address, remote_addr_len);
            arguments->client_address_len = remote_addr_len + 4;
            arguments->client_socket = client_socket;

            if (pthread_create(&thread_id, NULL, (void *) &_svr_connection_handler, (void *) arguments)) {
                // Log.
                log_message = memory_malloc(sizeof(char) * 256, 0);
                sprintf(log_message, "\t> Fatal ERROR during creating a new thread!\n");
                write_log(log_message);

                memory_free(log_message, 0);
                memory_free(arguments, 0);

                close(client_socket);
            }
        } else {
            // Log.
            log_message = memory_malloc(sizeof(char) * 256, 0);
            sprintf(log_message, "\t> Fatal ERROR during socket processing!\n");
            write_log(log_message);
            memory_free(log_message, 0);

            close(client_socket);

            exit(1);
        }
    }
}

/// Process received message from a client.
/// \param message      The message.
void _svr_process_request(char *message) {
    if (!message)
        return;

    char **tokens = _svr_split_message(message);
    char *id;
    player_t *player = NULL;
    game_t *game = NULL;

    if (tokens[0]) {
        id = tokens[0];
        player = player_find(id);

        if (!player) {
            _svr_count_bad_message(message);
            return;
        }

        game = player->game;

    } else {
        _svr_count_bad_message(message);
    }

    // Token list which is acceptable from client side.
    // List of events which server accepts from client side.
    if (tokens[1]) {
        if (strcmp(tokens[1], "get_games") == 0) {
            game_broadcast_update_games();

        } else if (strcmp(tokens[1], "create_new_game") == 0 && tokens[2]) {
            game_create(player, atoi(tokens[2]));

        } else if (strcmp(tokens[1], "join_player_to_game") == 0 && tokens[2]) {
            if (player_connect_to_game(player, player->game ? player->game : game_find(tokens[2]))) {
                // If player cannot join the game.
                char *msg = NULL;
                msg = memory_malloc(sizeof(char) * 256, 0);
                sprintf(msg, "%s;cannot_join_game\n", player->id); // Token message.
                svr_send(player->socket, msg, 0);
                memory_free(msg, 0);
            }

        } else if (strcmp(tokens[1], "disconnect_player") == 0) {
            if (player) {
                if (player->game)
                    player_disconnect_from_game(player, player->game);
                player_remove(player);
            }

        } else if (strcmp(tokens[1], "disconnect_player_from_game") == 0 && tokens[2]) {
            player_disconnect_from_game(player, game_find(tokens[2]));

        } else if (strcmp(tokens[1], "game_choice_selected") == 0 && tokens[2]) {
            game_logic_record_turn(player, atoi(tokens[2]));

        } else {
            _svr_count_bad_message(message);
        }

    } else {
        _svr_count_bad_message(message);
    }

    memory_free(tokens, 0);
}

/// Split a message to tokens.
/// \param message      The message.
/// \return             Split message / tokens.
char **_svr_split_message(char *message) {
    if (!message)
        return NULL;

    char **message_split = memory_malloc(MAX_CLIENT_TOKENS * sizeof(char *), 0);
    int i = 0;

    message_split[i] = strtok(message, ";");

    if (message_split[i] == NULL)
        return message_split;

    i++;
    while (i < MAX_CLIENT_TOKENS) {
        message_split[i] = strtok(NULL, ";");

        if (message_split[i] == NULL)
            return message_split;

        i++;
    }

    return message_split;
}

/// Call this if incorrect message received.
/// \param message      The message.
void _svr_count_bad_message(char *message) {
    printf("\t> Ignored message: \"%s\".\n", message);
    messages_bad++;
}

/// The main method of the application. It creates a server with entered port number. If the port number is not submited the server will be create with the port number 10000.
/// \param argv -
/// \param args -
/// \return Status code of success.
int main(int argv, char *args[]) {
    int port;
    char *log_message = NULL;
    char input[1024];
    time(&time_initial);

    colors_init();

    // Initialize log file.
    FILE *logs = fopen("server.log", "w");
    fclose(logs);

    // Log.
    log_message = memory_malloc(sizeof(char) * 256, 0);
    sprintf(log_message, "\t> Server is starting... /%s", asctime(localtime(&time_initial)));
    write_log(log_message);
    memory_free(log_message, 0);

    // Listen to custom port.
    if (argv > 1) {
        port = atoi(args[1]);

        if (port >= CUSTOM_PORT_LOWEST_POSSIBLE && port <= CUSTOM_PORT_HIGHEST_POSSIBLE) {
            printf("\t> Setting up the port (%d).\n", port);
        } else {
            printf("\t> Setting up the default port.\n");
            port = PORT_DEFAULT;
        }
    } else {
        printf("\t> Setting up the default port (%d).\n", PORT_DEFAULT);
        port = PORT_DEFAULT;
    }

    // Log.
    log_message = memory_malloc(sizeof(char) * 256, 0);
    sprintf(log_message, "\t> Server is running on the port: %d.\n", port);
    write_log(log_message);
    memory_free(log_message, 0);

    thread_id = 0;
    if (pthread_create(&thread_id, NULL, _svr_serve_connection, (void *) &port) != 0) {
        // Log.
        log_message = memory_malloc(sizeof(char) * 256, 0);
        sprintf(log_message, "\t> Fatal ERROR during creating a new thread!\n");
        write_log(log_message);
        memory_free(log_message, 0);
    }

    while (scanf("%s", input) != -1) {
        if (strcmp(input, "quit") == 0)
            break;
        if (strcmp(input, "info") == 0)
            print_info(stdout);
        if (strcmp(input, "memory") == 0)
            memory_print_status();
        if (strcmp(input, "games") == 0)
            game_print();
        if (strcmp(input, "players") == 0)
            player_print();
    }

    pthread_cancel(thread_id);

    colors_free();
    player_free();
    game_free();

    log_message = memory_malloc(sizeof(char) * 256, 0);
    sprintf(log_message, "\t> Server is shutting down.\n");
    write_log(log_message);
    memory_free(log_message, 0);

    write_stats();
    memory_print_status();

    print_info(stdout);

    return 0;
}