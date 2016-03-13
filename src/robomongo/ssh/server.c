#include "libssh2_config.h"
#include <libssh2.h>

#ifdef WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "log.h"

#ifndef INADDR_NONE
#define INADDR_NONE (in_addr_t)-1
#endif

#ifdef WIN32
#  define socket_type SOCKET
#  define socket_invalid INVALID_SOCKET
#else
#  define socket_type int
#  define socket_invalid (-1)
#endif

enum ssh_auth_type {
    AUTH_NONE = 0,
    AUTH_PASSWORD,
    AUTH_PUBLICKEY
};

/*
 * SSH Tunnel configuration
 */
struct ssh_tunnel_config {
    // Local IP and port to bind and listen to
    char *localip;
    unsigned int localport;

    // Username and password of remote user
    char *username;
    char *password;     // May be NULL or ""

    // Keys and optional passphrase
    char *privatekeyfile;
    char *publickeyfile;
    char *passphrase;   // May be NULL or ""

    // Remote IP and (host, port) in remote network
    char *serverip;
    unsigned int serverport;  // SSH port
    char *remotehost;   // Resolved by the remote server
    unsigned int remoteport;
};



/*
 * Initialises Sockets and Libssh
 * Returns 0 if initialization succeeds
 */
int init() {
    int err;
#ifdef WIN32
    WSADATA wsadata;
#endif

#ifdef WIN32
    err = WSAStartup(MAKEWORD(2,0), &wsadata);
    if (err != 0) {
        log_error("WSAStartup failed with error: %d", err);
        return 1;
    }
#endif

    err = libssh2_init (0);
    if (err != 0) {
        log_error("libssh2 initialization failed (%d)", err);
        return 1;
    }

    return 0;
}

/*
 * Cleanups Sockets and Libssh
 */
void cleanup() {
    libssh2_exit();

#ifdef WIN32
    WSACleanup();
#endif
}

/*
 * Returns socket if succeed, otherwise -1 on error
 */
socket_type socket_connect(char *ip, int port) {
    socket_type sock;
    struct sockaddr_in sin;

    /* Connect to SSH server */
    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == socket_invalid) {
        log_error("Failed to open socket");
        return -1;
    }

    sin.sin_family = AF_INET;
    if (INADDR_NONE == (sin.sin_addr.s_addr = inet_addr(ip))) {
        log_error("inet_addr error");
        return -1;
    }

    sin.sin_port = htons(port);
    if (connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
        log_error("Failed to connect to %s:%d", ip, port);
        return -1;
    }

    return sock;
}

/*
 * Returns socket (binded and in listen state) if succeed, otherwise -1 on error
 */
socket_type socket_listen(char *ip, int port) {
    socket_type listensock;
    struct sockaddr_in sin;
    int sockopt;
    socklen_t sinlen;

    listensock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listensock == socket_invalid) {
        log_error("Failed to open socket");
        return -1;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (INADDR_NONE == (sin.sin_addr.s_addr = inet_addr(ip))) {
        log_error("inet_addr");
        return -1;
    }

    sockopt = 1;
    setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
    sinlen = sizeof(sin);
    if (-1 == bind(listensock, (struct sockaddr *)&sin, sinlen)) {
        log_error("Cannot bind to port %d", port);
        return -1;
    }
    if (-1 == listen(listensock, 2)) {
        log_error("Failed to listen opened socket");
        return -1;
    }

    return listensock;

}

/*
 * Returns 0 if error.
 */
LIBSSH2_SESSION *ssh_connect(socket_type sock, enum ssh_auth_type type, char *username, char *password,
                             char *publickeypath, char *privatekeypath, char *passphrase) {
    int rc, i, auth = AUTH_NONE;
    LIBSSH2_SESSION *session;
    const char *fingerprint;
    char *userauthlist;

    /* Create a session instance */
    session = libssh2_session_init();
    if (!session) {
        log_error("Could not initialize SSH session");
        return 0;
    }

    /* ... start it up. This will trade welcome banners, exchange keys,
     * and setup crypto, compression, and MAC layers
     */
    rc = libssh2_session_handshake(session, sock);
    if (rc) {
        log_error("Error when starting up SSH session: %d\n", rc);
        return 0;
    }

    /* At this point we havn't yet authenticated.  The first thing to do
     * is check the hostkey's fingerprint against our known hosts Your app
     * may have it hard coded, may go to a file, may present it to the
     * user, that's your call
     */
    fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);

    /*
    fprintf(stderr, "Fingerprint: ");
    for(i = 0; i < 20; i++)
        fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
    fprintf(stderr, "\n");
    */

    /* check what authentication methods are available */
    userauthlist = libssh2_userauth_list(session, username, strlen(username));
    log_msg("Authentication methods: %s", userauthlist);
    if (strstr(userauthlist, "password"))
        auth |= AUTH_PASSWORD;
    if (strstr(userauthlist, "publickey"))
        auth |= AUTH_PUBLICKEY;

    // If authentication by password is available
    // and was chosen by the user, then use it
    if (auth & AUTH_PASSWORD && type == AUTH_PASSWORD) {
        if (libssh2_userauth_password(session, username, password)) {
            log_error("Authentication by password failed");
            return 0;
        }
        log_msg("Authentication by password succeeded.");
    // If authentication by key is available
    // and was chosen by the user, then use it
    } else if (auth & AUTH_PUBLICKEY && type == AUTH_PUBLICKEY) {
        if (libssh2_userauth_publickey_fromfile(session, username, publickeypath,
                                                privatekeypath, passphrase)) {
            log_error("Authentication by key failed");
            return 0;
        }
        log_msg("Authentication by key succeeded.");
    } else {
        log_error("No supported authentication methods found.");
        return 0;
    }

    return session;
}


int main(int argc, char *argv[]) {
    struct ssh_tunnel_config config;
    int err = 0;

    config.localip = "127.0.0.1";
    config.localport = 27040;
    config.username = "dmitry";
    config.password = "";
    config.privatekeyfile = "/Users/dmitry/.ssh/ubuntik";
    config.publickeyfile = "/Users/dmitry/.ssh/ubuntik.pub";
    config.passphrase = "";
    config.serverip = "198.61.166.171";
    config.serverport = 22;
    config.remotehost = "localhost";
    config.remoteport = 27017;

    err = init();
    if (err) {
        // errors are already logged by init
        return 1;
    }

    log_msg("Connecting to %s...", config.serverip);
    socket_type ssh_socket = socket_connect(config.serverip, config.serverport);
    if (ssh_socket == -1) {
        // errors are already logged by socket_connect
        return 1;
    }

    LIBSSH2_SESSION *session = ssh_connect(ssh_socket, AUTH_PUBLICKEY, config.username, config.password,
                                           config.publickeyfile, config.privatekeyfile, config.passphrase);
    if (session == 0) {
        // errors are already logged by ssh_connect
        return 1;
    }

    socket_type local_socket = socket_listen(config.localip, config.localport);
    if (local_socket == -1) {
        // errors are already logged by socket_listen
        return 1;
    }

    log_msg("Waiting for TCP connection on %s:%d...", config.localip, config.localport);

    cleanup();

    return 0;
}
