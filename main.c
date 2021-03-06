#define HAVE_STDARG_H 1
#define WS_MSVC_NORETURN
#define _GNU_SOURCE


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <epan/epan.h>
#include <epan/print.h>
#include <epan/timestamp.h>
#include <epan/epan-int.h>
#include <epan/epan_dissect.h>
#include <epan/proto.h>
#include <epan/ftypes/ftypes.h>
#include <epan/asm_utils.h>
#include <glib.h>


#ifdef LINT
#define gboolean _Bool
#define guint8  uint8_t
#define guint16 uint16_t
#define guint32 uint32_t
#define gint64  int64_t
#define TRUE true
#define FALSE false
#endif

extern tvbuff_t *frame_tvbuff_new(const frame_data *fd, const guint8 *buf);
static const nstime_t *tshark_get_frame_ts(void *data, guint32 frame_num);
static void clean();

enum print_type {
    PRINT_MANUAL,
    PRINT_TEXT,
};

//global variable
capture_file cfile;

int init(char *filename)
{
    int          err = 0;
    char       *err_info = NULL;

#if 0
    epan_register_plugin_types(); /* Types known to libwireshark */
    scan_plugins();
    /* Register all libwiretap plugin modules. */
    register_all_wiretap_modules();
#endif
    wtap_register_plugin_types(); /* Types known to libwiretap */

    if (!epan_init(register_all_protocols, register_all_protocol_handoffs, NULL, NULL)) {
        fprintf(stderr, "Error at epan_init\n");
        return 2;
    }

    proto_initialize_all_prefixes();

    cap_file_init(&cfile);
    cfile.filename = filename;

    cfile.wth = wtap_open_offline(cfile.filename, WTAP_TYPE_AUTO, &err, &err_info, TRUE);
    if (cfile.wth == NULL)
        goto fail;

    cfile.count = 0;
    epan_free(cfile.epan);
    cfile.epan = epan_new();
    cfile.epan->data = &cfile;

    cfile.epan->get_frame_ts = tshark_get_frame_ts;
    timestamp_set_precision(TS_PREC_AUTO);

    cfile.frames = new_frame_data_sequence();


    return 0;

fail:
    clean();
    return err;
}

int read_packet(epan_dissect_t **edt_r)
{
    epan_dissect_t    *edt;
    int                err;
    char              *err_info = NULL;
    static guint32     cum_bytes = 0;
    static gint64      data_offset = 0;

    struct wtap_pkthdr *whdr = wtap_phdr(cfile.wth);
    unsigned char      *buf = wtap_buf_ptr(cfile.wth);

    edt = epan_dissect_new(cfile.epan, TRUE, TRUE);

    if (wtap_read(cfile.wth, &err, &err_info, &data_offset)) {

        cfile.count++;

        frame_data fdlocal;
        frame_data_init(&fdlocal, cfile.count, whdr, data_offset, cum_bytes);

        frame_data_set_before_dissect(&fdlocal, &cfile.elapsed_time, &cfile.ref, cfile.prev_dis);

        epan_dissect_run_with_taps(edt, cfile.cd_t, whdr, frame_tvbuff_new(&fdlocal, buf), &fdlocal, NULL);

        frame_data_set_after_dissect(&fdlocal, &cum_bytes);
        cfile.prev_cap = cfile.prev_dis = frame_data_sequence_add(cfile.frames, &fdlocal);

        //free space
        frame_data_destroy(&fdlocal);

        *edt_r = edt;
        return TRUE;
    }
    return FALSE;
}

void clean()
{
    if (cfile.frames != NULL) {
        free_frame_data_sequence(cfile.frames);
        cfile.frames = NULL;
    }

    if (cfile.wth != NULL) {
        wtap_close(cfile.wth);
        cfile.wth = NULL;
    }

    if (cfile.epan != NULL)
        epan_free(cfile.epan);

    epan_cleanup();
}

void visit(proto_node *node, gpointer data) {
    field_info *fi  = PNODE_FINFO(node);
    if (!fi || !fi->rep) return;

    printf("***\t%s\n", node->finfo->rep->representation);

    g_assert((fi->tree_type >= -1) && (fi->tree_type < num_tree_types));
    if (node->first_child != NULL) {
        proto_tree_children_foreach(node, visit, data);
    }
}
void print_each_packet_manual()
{
    epan_dissect_t *edt;

    while (read_packet(&edt)) {
        proto_tree_children_foreach(edt->tree, visit, NULL);

        epan_dissect_free(edt);
        edt = NULL;
    }
}

void print_each_packet_text()
{
    static int num;
    epan_dissect_t *edt;
    print_args_t    print_args = {0};
    print_stream_t *print_stream = print_stream_text_stdio_new(stdout);

    print_args.print_hex = FALSE;
    print_args.print_dissections = print_dissections_expanded;

    while (read_packet(&edt)) {

        proto_tree_print(&print_args, edt, NULL, print_stream);

        epan_dissect_free(edt);
        edt = NULL;
    }
}


static const nstime_t * tshark_get_frame_ts(void *data, guint32 frame_num)
{
    capture_file *cf = (capture_file *) data;

    if (cf->ref && cf->ref->num == frame_num)
        return &(cf->ref->abs_ts);

    if (cf->prev_dis && cf->prev_dis->num == frame_num)
        return &(cf->prev_dis->abs_ts);

    if (cf->prev_cap && cf->prev_cap->num == frame_num)
        return &(cf->prev_cap->abs_ts);

    if (cf->frames) {
        frame_data *fd = frame_data_sequence_find(cf->frames, frame_num);

        return (fd) ? &fd->abs_ts : NULL;
    }

    return NULL;
}

    void
cap_file_init(capture_file *cf)
{
    /* Initialize the capture file struct */
    memset(cf, 0, sizeof(capture_file));
    cf->snap            = WTAP_MAX_PACKET_SIZE;
}

void print_usage(char *argv[])
{
    printf("Usage: %s -f <input_file> ", argv[0]);
    printf("[-t <manual|text> (default text)]\n");
}

int main(int argc, char* argv[])
{

    int          err;
    char        *filename = NULL;
    enum print_type print_type = PRINT_TEXT;
    int          opt;


    while((opt = getopt(argc, argv, "f:t:")) != -1) {
        switch(opt) {
            case 'f':
                filename = strdup(optarg);
                break;
            case 't':
                if (strcmp(optarg, "manual") == 0) {
                    print_type = PRINT_MANUAL;
                } else if (strcmp(optarg, "text") == 0) {
                    print_type = PRINT_TEXT;
                } else {
                    print_type = PRINT_TEXT;
                }
                break;
            default:
                print_usage(argv);
                return 1;
        }
    }


    if (filename == NULL) {
        print_usage(argv);
        return 1;
    }

    if (access(filename, F_OK) == -1) {
        fprintf(stderr, "File '%s' doesn't exist.\n", filename);
        return 1;
    }


    err = init(filename);
    if (err) {
        return 1;
    }

    switch (print_type) {
        case PRINT_MANUAL:
            print_each_packet_manual();
            break;
        case PRINT_TEXT:
            print_each_packet_text();
            break;
    }

    clean();
    return 0;
}

