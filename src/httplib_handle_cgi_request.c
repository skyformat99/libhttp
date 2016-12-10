/* 
 * Copyright (c) 2016 Lammert Bies
 * Copyright (c) 2013-2016 the Civetweb developers
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */



#include "libhttp-private.h"



/*
 * void XX_httplib_handle_cgi_request( struct mg_connection *conn, const char *prog );
 *
 * The function XX_httplib_handle_cgi_request() handles a request for a CGI
 * resource.
 */

#if !defined(NO_CGI)

void XX_httplib_handle_cgi_request( struct mg_connection *conn, const char *prog ) {

	char *buf;
	size_t buflen;
	int headers_len;
	int data_len;
	int i;
	int truncated;
	int fdin[2]  = {-1, -1};
	int fdout[2] = {-1, -1};
	int fderr[2] = {-1, -1};
	const char *status;
	const char *status_text;
	const char *connection_state;
	char *pbuf;
	char dir[PATH_MAX];
	char *p;
	struct mg_request_info ri;
	struct cgi_environment blk;
	FILE *in;
	FILE *out;
	FILE *err;
	struct file fout = STRUCT_FILE_INITIALIZER;
	pid_t pid = (pid_t)-1;

	if (conn == NULL) return;

	in  = NULL;
	out = NULL;
	err = NULL;

	buf    = NULL;
	buflen = 16384;
	XX_httplib_prepare_cgi_environment(conn, prog, &blk);

	/* CGI must be executed in its own directory. 'dir' must point to the
	 * directory containing executable program, 'p' must point to the
	 * executable program name relative to 'dir'. */
	XX_httplib_snprintf(conn, &truncated, dir, sizeof(dir), "%s", prog);

	if (truncated) {
		mg_cry(conn, "Error: CGI program \"%s\": Path too long", prog);
		XX_httplib_send_http_error(conn, 500, "Error: %s", "CGI path too long");
		goto done;
	}

	if ((p = strrchr(dir, '/')) != NULL) {
		*p++ = '\0';
	} else {
		dir[0] = '.', dir[1] = '\0';
		p = (char *)prog;
	}

	if (pipe(fdin) != 0 || pipe(fdout) != 0 || pipe(fderr) != 0) {
		status = strerror(ERRNO);
		mg_cry(conn, "Error: CGI program \"%s\": Can not create CGI pipes: %s", prog, status);
		XX_httplib_send_http_error(conn, 500, "Error: Cannot create CGI pipe: %s", status);
		goto done;
	}

	pid = XX_httplib_spawn_process(conn, p, blk.buf, blk.var, fdin, fdout, fderr, dir);

	if (pid == (pid_t)-1) {
		status = strerror(ERRNO);
		mg_cry(conn, "Error: CGI program \"%s\": Can not spawn CGI process: %s", prog, status);
		XX_httplib_send_http_error(conn, 500, "Error: Cannot spawn CGI process [%s]: %s", prog, status);
		goto done;
	}

	/* Make sure child closes all pipe descriptors. It must dup them to 0,1 */
	XX_httplib_set_close_on_exec( (SOCKET)fdin[0],  conn );  /* stdin read */
	XX_httplib_set_close_on_exec( (SOCKET)fdout[1], conn ); /* stdout write */
	XX_httplib_set_close_on_exec( (SOCKET)fderr[1], conn ); /* stderr write */
	XX_httplib_set_close_on_exec( (SOCKET)fdin[1],  conn );  /* stdin write */
	XX_httplib_set_close_on_exec( (SOCKET)fdout[0], conn ); /* stdout read */
	XX_httplib_set_close_on_exec( (SOCKET)fderr[0], conn ); /* stderr read */

	/* Parent closes only one side of the pipes.
	 * If we don't mark them as closed, close() attempt before
	 * return from this function throws an exception on Windows.
	 * Windows does not like when closed descriptor is closed again. */
	close( fdin[0]  );
	close( fdout[1] );
	close( fderr[1] );

	fdin[0]  = -1;
	fdout[1] = -1;
	fderr[1] = -1;

	if ((in = fdopen(fdin[1], "wb")) == NULL) {
		status = strerror(ERRNO);
		mg_cry(conn, "Error: CGI program \"%s\": Can not open stdin: %s", prog, status);
		XX_httplib_send_http_error(conn, 500, "Error: CGI can not open fdin\nfopen: %s", status);
		goto done;
	}

	if ((out = fdopen(fdout[0], "rb")) == NULL) {
		status = strerror(ERRNO);
		mg_cry(conn, "Error: CGI program \"%s\": Can not open stdout: %s", prog, status);
		XX_httplib_send_http_error(conn, 500, "Error: CGI can not open fdout\nfopen: %s", status);
		goto done;
	}

	if ((err = fdopen(fderr[0], "rb")) == NULL) {
		status = strerror(ERRNO);
		mg_cry(conn, "Error: CGI program \"%s\": Can not open stderr: %s", prog, status);
		XX_httplib_send_http_error(conn, 500, "Error: CGI can not open fdout\nfopen: %s", status);
		goto done;
	}

	setbuf( in,  NULL );
	setbuf( out, NULL );
	setbuf( err, NULL );
	fout.fp = out;

	if ((conn->request_info.content_length > 0) || conn->is_chunked) {
		/* This is a POST/PUT request, or another request with body data. */
		if (!XX_httplib_forward_body_data(conn, in, INVALID_SOCKET, NULL)) {
			/* Error sending the body data */
			mg_cry(conn, "Error: CGI program \"%s\": Forward body data failed", prog);
			goto done;
		}
	}

	/* Close so child gets an EOF. */
	fclose(in);
	in = NULL;
	fdin[1] = -1;

	/* Now read CGI reply into a buffer. We need to set correct
	 * status code, thus we need to see all HTTP headers first.
	 * Do not send anything back to client, until we buffer in all
	 * HTTP headers. */
	data_len = 0;
	buf = (char *)XX_httplib_malloc(buflen);
	if (buf == NULL) {
		XX_httplib_send_http_error(conn, 500, "Error: Not enough memory for CGI buffer (%u bytes)", (unsigned int)buflen);
		mg_cry(conn, "Error: CGI program \"%s\": Not enough memory for buffer (%u " "bytes)", prog, (unsigned int)buflen);
		goto done;
	}
	headers_len = XX_httplib_read_request(out, conn, buf, (int)buflen, &data_len);
	if (headers_len <= 0) {

		/* Could not parse the CGI response. Check if some error message on
		 * stderr. */
		i = XX_httplib_pull_all(err, conn, buf, (int)buflen);
		if (i > 0) {
			mg_cry(conn, "Error: CGI program \"%s\" sent error " "message: [%.*s]", prog, i, buf);
			XX_httplib_send_http_error(conn, 500, "Error: CGI program \"%s\" sent error " "message: [%.*s]", prog, i, buf);
		} else {
			mg_cry(conn, "Error: CGI program sent malformed or too big " "(>%u bytes) HTTP headers: [%.*s]", (unsigned)buflen, data_len, buf);

			XX_httplib_send_http_error(conn,
			                500,
			                "Error: CGI program sent malformed or too big "
			                "(>%u bytes) HTTP headers: [%.*s]",
			                (unsigned)buflen,
			                data_len,
			                buf);
		}

		goto done;
	}
	pbuf = buf;
	buf[headers_len - 1] = '\0';
	XX_httplib_parse_http_headers(&pbuf, &ri);

	/* Make up and send the status line */
	status_text = "OK";
	if ((status = XX_httplib_get_header(&ri, "Status")) != NULL) {
		conn->status_code = atoi(status);
		status_text = status;
		while (isdigit(*(const unsigned char *)status_text)
		       || *status_text == ' ') {
			status_text++;
		}
	} else if (XX_httplib_get_header(&ri, "Location") != NULL) {
		conn->status_code = 302;
	} else {
		conn->status_code = 200;
	}
	connection_state = XX_httplib_get_header(&ri, "Connection");
	if (!XX_httplib_header_has_option(connection_state, "keep-alive")) {
		conn->must_close = 1;
	}
	mg_printf(conn, "HTTP/1.1 %d %s\r\n", conn->status_code, status_text);

	/* Send headers */
	for (i = 0; i < ri.num_headers; i++) {
		mg_printf(conn, "%s: %s\r\n", ri.http_headers[i].name, ri.http_headers[i].value);
	}
	mg_write(conn, "\r\n", 2);

	/* Send chunk of data that may have been read after the headers */
	conn->num_bytes_sent +=
	    mg_write(conn, buf + headers_len, (size_t)(data_len - headers_len));

	/* Read the rest of CGI output and send to the client */
	XX_httplib_send_file_data(conn, &fout, 0, INT64_MAX);

done:
	XX_httplib_free(blk.var);
	XX_httplib_free(blk.buf);

	if (pid != (pid_t)-1) {
		kill(pid, SIGKILL);
#if !defined(_WIN32)
		{
			int st;
			while (waitpid(pid, &st, 0) != -1)
				; /* clean zombies */
		}
#endif
	}
	if ( fdin[0]  != -1 ) close( fdin[0]  );
	if ( fdout[1] != -1 ) close( fdout[1] );

	if ( in  != NULL ) fclose( in  ); else if ( fdin[1]  != -1 ) close( fdin[1]  );
	if ( out != NULL ) fclose( out ); else if ( fdout[0] != -1 ) close( fdout[0] );
	if ( err != NULL ) fclose( err ); else if ( fderr[0] != -1 ) close( fderr[0] );

	if ( buf != NULL ) XX_httplib_free( buf );

}  /* XX_httplib_handle_cgi_request */

#endif /* !NO_CGI */
