/*
 * Description: GET a url with nw_req and print what comes back.
 *              req_get 'http://127.0.0.1:8080/hello?x=1' [timeout]
 */

# include <stdio.h>
# include <stdlib.h>

# include "nw_req.h"
# include "nw_evt.h"

static int result = 1;

static void on_response(int error, nw_req_rsp *rsp, void *privdata)
{
    if (error) {
        fprintf(stderr, "error %d: %s\n", error, rsp->message);
    } else {
        fprintf(stderr, "status: %d\n", rsp->status);
        for (uint32_t i = 0; i < rsp->header_count; ++i)
            fprintf(stderr, "%s: %s\n", rsp->headers[i].name, rsp->headers[i].value);
        fwrite(rsp->body, 1, rsp->body_size, stdout);
        result = rsp->status == 200 ? 0 : 2;
    }
    nw_loop_break();
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: %s url [timeout]\n", argv[0]);
        exit(0);
    }
    if (nw_req_get(argv[1], argc > 2 ? atof(argv[2]) : 0, on_response, NULL) == NULL) {
        fprintf(stderr, "not a http url: %s\n", argv[1]);
        exit(1);
    }
    /* a program that has other things to do goes on with them here */
    nw_loop_run();
    nw_loop_release();
    return result;
}
