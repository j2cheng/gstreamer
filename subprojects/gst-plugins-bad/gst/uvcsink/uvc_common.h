#pragma once

int socket_create(void);
int socket_destroy(const char *path, int fd);
int socket_bind(int fd, const char *path);
int socket_connect(int fd, const char *path);
