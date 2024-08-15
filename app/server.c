#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <zlib.h>

void *handleClient(void *client_socket);
void parseRequest(int socket_fd, char* httpMethod, char *requestTarget, char* headers, char* httpBody);
const char *directory = NULL;

int main(int argc, char *argv[]) {
	for(int i = 0; i < argc; i++) {
		if(strcmp(argv[i], "--directory") == 0 && i + 1 < argc) {
			directory = argv[i + 1];
			i++;
		}
	}

	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

	// Uncomment this block to pass the first stage
	
	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;
	
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}
	
	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}
	
	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
									 .sin_port = htons(4221),
									 .sin_addr = { htonl(INADDR_ANY) },
									};
	
	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}
	
	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}
	
	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);
	
	while(1) {
	   	int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);

		if(client_fd == -1) {
			continue;
		}

		pthread_t new_process;
		pthread_create(&new_process, NULL, handleClient, &client_fd);
	}
	
	close(server_fd);

	return 0;
}

void *handleClient(void *client_socket) {
	int client_fd = *(int *)client_socket;

	char *buffer = malloc(2048);
	read(client_fd, buffer, 2048);

	char *httpRequestLine = strtok(buffer, "\r\n");
	char *httpHeaders = buffer + strlen(httpRequestLine) + 2;
	char *httpHeadersEnd = strstr(httpHeaders, "\r\n\r\n");
	char *httpBody = httpHeadersEnd + 4; 

	if (httpHeadersEnd != NULL)
	{
		*httpHeadersEnd = '\0';
	}

	char *httpMethod = strtok(httpRequestLine, " ");
	char *httpRequestTarget = strtok(NULL, " ");
	char *httpVersion = strtok(NULL, " ");

	parseRequest(client_fd, httpMethod, httpRequestTarget, httpHeaders, httpBody);
	free(buffer);
}

int gzip_compress(const char *src, size_t src_len, char **dest, size_t *dest_len) {
    z_stream stream;
    int ret;

    // Allocate output buffer
    *dest_len = compressBound(src_len) + 18;  // 18 is added for the gzip header/footer
    *dest = malloc(*dest_len);
    if (*dest == NULL) {
        return Z_MEM_ERROR;
    }

    // Initialize the zlib stream for gzip
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)src;
    stream.avail_in = src_len;
    stream.next_out = (Bytef *)*dest;
    stream.avail_out = *dest_len;

    ret = deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        free(*dest);
        return ret;
    }

    // Perform the compression
    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        free(*dest);
        return ret == Z_OK ? Z_BUF_ERROR : ret;
    }

    *dest_len = stream.total_out;

    deflateEnd(&stream);
    return Z_OK;
}

void parseRequest(int socket_fd, char* httpMethod, char *requestTarget, char* headers, char* httpBody) {
	char *notFoundReply = "HTTP/1.1 404 Not Found\r\n\r\n";
	char *okReply = "HTTP/1.1 200 OK\r\n\r\n";

	if(strcmp(requestTarget, "/") == 0 || strcmp(requestTarget, "/index.html") == 0) {
		send(socket_fd, okReply, strlen(okReply), 0);
		return;
	}

	if(strstr(requestTarget, "echo") != NULL) {
		char *returnBody = requestTarget + strlen("/echo/");
		size_t returnBodyLen = strlen(returnBody);
		char *compressedBody = NULL;
		size_t compressedBodyLen = 0;
		char replyBuffer[1024];

		char *encodingHeaderFlag = strstr(headers, "Accept-Encoding: ");
		printf("Encoding header: %s\n", encodingHeaderFlag);

		// Initialize the HTTP response
		snprintf(replyBuffer, sizeof(replyBuffer),
				 "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n");

		// Check if gzip compression is requested
		if (encodingHeaderFlag != NULL && strstr(encodingHeaderFlag, "gzip") != NULL)
		{
			printf("Gzip encoding\n");
			int result = gzip_compress(returnBody, returnBodyLen, &compressedBody, &compressedBodyLen);
			if (result != Z_OK)
			{
				printf("Compression failed with error code: %d\n", result);
				send(socket_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 40, 0);
				return;
			}
			// Add Content-Encoding and Content-Length headers
			snprintf(replyBuffer + strlen(replyBuffer),
					 sizeof(replyBuffer) - strlen(replyBuffer),
					 "Content-Encoding: gzip\r\nContent-Length: %zu\r\n\r\n", compressedBodyLen);
			// Send the header and compressed body
			send(socket_fd, replyBuffer, strlen(replyBuffer), 0);
			send(socket_fd, compressedBody, compressedBodyLen, 0);
			free(compressedBody);
		}
		else
		{
			// No compression, send the plain text
			snprintf(replyBuffer + strlen(replyBuffer),
					 sizeof(replyBuffer) - strlen(replyBuffer),
					 "Content-Length: %zu\r\n\r\n%s", returnBodyLen, returnBody);
			send(socket_fd, replyBuffer, strlen(replyBuffer), 0);
		}
		return;
	}

	if(strstr(requestTarget, "files") != NULL && strcmp(httpMethod, "GET") == 0) {
		char *fileName = requestTarget + strlen("/files/");

		int file_fd = open(strcat(directory, fileName), O_RDONLY);

		if(file_fd == -1) {
			printf("File not found\n");
			send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
			close(file_fd);
			return;
		}

		struct stat fileStat;
		if(fstat(file_fd, &fileStat) == -1) {
			send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
			close(file_fd);
			return;
		}

		char *fileBuffer = malloc(fileStat.st_size);
		ssize_t bytesRead = read(file_fd, fileBuffer, fileStat.st_size);
		
		if(bytesRead == -1) {
			send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
			close(file_fd);
			free(fileBuffer);
			return;
		}

		char replyBuffer[1024 + fileStat.st_size];
		snprintf(
			replyBuffer, 
			sizeof(replyBuffer), 
			"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %ld\r\n\r\n%s", fileStat.st_size, fileBuffer
		);

		send(socket_fd, replyBuffer, strlen(replyBuffer), 0);

		close(file_fd);
		free(fileBuffer);
		return;
	}

	if(strstr(requestTarget, "files") != NULL && strcmp(httpMethod, "POST") == 0) {
		char *fileName = requestTarget + strlen("/files/");

		printf("File name: %s\n", fileName);
		printf("Directory: %s\n", directory);
		printf("Body: %s\n", httpBody);

		int file_fd = open(strcat(directory, fileName), O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if(file_fd == -1) {
			send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
			close(file_fd);
			return;
		}

		ssize_t bytesWritten = write(file_fd, httpBody, strlen(httpBody));
		if(bytesWritten == -1) {
			send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
			close(file_fd);
			return;
		}

		char* createSuccess = "HTTP/1.1 201 Created\r\n\r\n";
		send(socket_fd, createSuccess, strlen(createSuccess), 0);
	}

	if(strstr(requestTarget, "user-agent") != NULL) {
		char replyBuffer[1024];

		char *host = strtok(headers, "\r\n");
		char *userAgent = strtok(NULL, "\r\n");
		char *subStr = userAgent + strlen("User-Agent: ");

		snprintf(replyBuffer, sizeof(replyBuffer), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n%s", strlen(subStr), subStr);

		send(socket_fd, replyBuffer, strlen(replyBuffer), 0);

		return;
	}

	send(socket_fd, notFoundReply, strlen(notFoundReply), 0);
}
