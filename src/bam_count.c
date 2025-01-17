// count reads/fragments matrix for single-cell datasets
#include "utils.h"
#include "number.h"
#include "dict.h"
#include "dna_pool.h"
#include "htslib/khash.h"
#include "htslib/kstring.h"
#include "htslib/sam.h"
#include "htslib/bgzf.h"
#include <sys/stat.h>
#include "pisa_version.h" // mex output

// from v0.10, -ttype supported
#include "read_anno.h"
#include "biostring.h"

// accept file list
#include "bam_files.h"

static struct args {
    const char *input_fname;
    const char *whitelist_fname;
    const char *output_fname;
    const char *outdir; // v0.4, support Market Exchange Format (MEX) for sparse matrices
        
    const char *tag; // cell barcode tag
    const char *anno_tag; // feature tag
    const char *umi_tag;

    const char *prefix;

    const char *sample_list;
    
    struct dict *features;
    struct dict *barcodes;
    
    int mapq_thres;
    int use_dup;
    int enable_corr_umi;
    int n_thread;
    int one_hit;
    
    //htsFile *fp_in;
    //bam_hdr_t *hdr;

    uint64_t n_record;
    uint64_t n_record1;
    uint64_t n_record2;
    int alias_file_cb;
    
    const char *region_type_tag;
    int n_type;
    enum exon_type *region_types;

    int velocity;
    
    struct bam_files *files;
} args = {
    .input_fname     = NULL,
    .whitelist_fname = NULL,
    .output_fname    = NULL,
    .outdir          = NULL,
    .tag             = NULL,
    .anno_tag        = NULL,
    .umi_tag         = NULL,

    .prefix          = NULL,
    .barcodes        = NULL,
    .features        = NULL,

    .mapq_thres      = 20,
    .use_dup         = 0,
    .enable_corr_umi = 0,
    .one_hit         = 0,
    .n_thread        = 5,
    //.fp_in           = NULL,
    //.hdr             = NULL,
    .n_record        = 0,
    .n_record1       = 0,
    .n_record2       = 0,
    .alias_file_cb   = 0,
    
    .region_type_tag = "RE",
    .n_type          = 0,
    .region_types    = NULL,
    .velocity        = 0,
    .files           = NULL,
};

struct counts {
    uint32_t count;
    struct PISA_dna_pool *p;

    uint32_t unspliced;
    struct PISA_dna_pool *up;
};

static void memory_release()
{
    //bam_hdr_destroy(args.hdr);
    //sam_close(args.fp_in);

    close_bam_files(args.files);
    
    int i;
    int n_feature;
    n_feature = dict_size(args.features);
    for (i = 0; i < n_feature; ++i) {
        struct PISA_dna_pool *v = dict_query_value(args.features, i);
        PISA_idx_destroy(v);
    }
    dict_destroy(args.features);
    dict_destroy(args.barcodes);
}

extern int bam_count_usage();

static int parse_args(int argc, char **argv)
{
    int i;
    const char *mapq = NULL;
    const char *n_thread = NULL;
    const char *region_types = NULL;
    for (i = 1; i < argc;) {
        const char *a = argv[i++];
        const char **var = 0;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 1;
        if (strcmp(a, "-tag") == 0 || strcmp(a, "-cb") == 0) var = &args.tag;
        else if (strcmp(a, "-anno-tag") == 0) var = &args.anno_tag;
        else if (strcmp(a, "-list") == 0) var = &args.whitelist_fname;
        else if (strcmp(a, "-umi") == 0) var = &args.umi_tag;
        else if (strcmp(a, "-o") == 0) var = &args.output_fname;
        else if (strcmp(a, "-outdir") == 0) var = &args.outdir;
        else if (strcmp(a, "-q") == 0) var = &mapq;
        else if (strcmp(a, "-@") == 0) var = &n_thread;
        else if (strcmp(a, "-ttag") == 0) var = &args.region_type_tag;
        else if (strcmp(a, "-ttype") == 0) var = &region_types;
        else if (strcmp(a, "-prefix") == 0) var = &args.prefix;
        else if (strcmp(a, "-sample-list") == 0) var = &args.sample_list;
        else if (strcmp(a, "-dup") == 0) {
            args.use_dup = 1;
            continue;
        }
        else if (strcmp(a, "-velo") == 0) {
            args.velocity = 1;
            continue;
        }
        else if (strcmp(a, "-one-hit") == 0) {
            args.one_hit = 1;
            continue;
        }
        
        else if (strcmp(a, "-corr") == 0) {
            //args.enable_corr_umi = 1;
            warnings("Option -corr has been removed since v0.8, to correct UMIs please use `PISA corr` instead.");
            continue;
        }

        else if (strcmp(a, "-file-barcode") ==0) {
            args.alias_file_cb = 1;
            continue;
        }        
        
        if (var != 0) {
            if (i == argc) error("Miss an argument after %s.", a);
            *var = argv[i++];
            continue;
        }

        if (args.input_fname == NULL) {
            args.input_fname = a;
            continue;
        }
        error("Unknown argument, %s", a);
    }

    if (args.input_fname == 0 && args.sample_list == NULL) error("No input bam.");
    if (args.input_fname && args.sample_list) error("Input bam conflict with -sample-list.");
    
    if (args.output_fname) {
        warnings("PISA now support MEX format. Old cell X gene expression format is very poor performance. Try -outdir instead of -o.");
    }
    
    if (args.tag == 0 && args.alias_file_cb == 0)
        error("No cell barcode specified and -file-barcode disabled.");

    if (args.anno_tag == 0) error("No anno tag specified.");

    if (n_thread) args.n_thread = str2int((char*)n_thread);

    if (args.outdir) {
         struct stat sb;
         if (stat(args.outdir, &sb) != 0) error("Directory %s is not exist.", args.outdir);
         if (S_ISDIR(sb.st_mode) == 0)  error("%s does not look like a directory.", args.outdir);
    }

    if (args.input_fname) {
        args.files = init_bam_line(args.input_fname, args.n_thread);
    }
    else if (args.sample_list) {
        args.files = init_bam_list(args.sample_list, args.n_thread);
    }
    else error("Not found input bam file.");

    if (mapq) {
        args.mapq_thres = str2int(mapq);        
    }
    args.features = dict_init();
    dict_set_value(args.features);
    
    args.barcodes = dict_init();

    if (args.whitelist_fname) {
        dict_read(args.barcodes, args.whitelist_fname);
        if (dict_size(args.barcodes) == 0) error("Barcode list is empty?");
    }

    if (region_types) {
        kstring_t str = {0,0,0};
        int n = 0;
        kputs(region_types, &str);
        int *s = str_split(&str, &n);
        if (n == 0) error("Failed to parse -ttype, %s", region_types);
        args.n_type = n;
        args.region_types = malloc(n*sizeof(enum exon_type));
        int k;
        for (k = 0; k < n; ++k) {
            char *rt = str.s+s[k];
            if (strlen(rt) != 1) error("Failed to parse -ttype, %s", region_types);
            enum exon_type type = RE_type_map(rt[0]);
            if (type == type_unknown) error("Unknown type %s", rt);
            args.region_types[k] = type;
        }
        free(s);
        free(str.s);
    }
    return 0;
}

int count_matrix_core(bam1_t *b, char *tag)
{
    if (args.tag) {
        uint8_t *tag0 = bam_aux_get(b, args.tag);
        if (!tag0) return 1;

        tag = (char*)(tag0+1);
    }
    else if (args.alias_file_cb) {
        assert(tag);
    }
    
    uint8_t *anno_tag = bam_aux_get(b, args.anno_tag);
    if (!anno_tag) return 1;

    if (args.umi_tag) {
        uint8_t *umi_tag = bam_aux_get(b, args.umi_tag);
        if (!umi_tag) return 1;
    }

    int unspliced = 0;
    if (args.velocity) {
        uint8_t *data = bam_aux_get(b, args.region_type_tag);
        if (!data) return 1;
        if (RE_type_map(data[1]) == type_unknown) return 1;
        if (RE_type_map(data[1]) == type_antisense) return 1;
        if (RE_type_map(data[1]) == type_ambiguous) return 1;
        if (RE_type_map(data[1]) == type_intergenic)  return 1;
        if (RE_type_map(data[1]) == type_exon_intron || RE_type_map(data[1]) == type_intron)
            unspliced = 1;
    }
    
    int cell_id;
    if (args.whitelist_fname) {
        cell_id = dict_query(args.barcodes, tag);
        if (cell_id == -1) return 1;
    }
    else {
        cell_id = dict_push(args.barcodes, tag);
    }

    // for each feature
    kstring_t str = {0,0,0};
    kputs((char*)(anno_tag+1), &str);
    int n_gene;
    int *s = str_split(&str, &n_gene); // seperator ; or ,

    // Sometime two or more genes or functional regions can overlapped with each other, if default PISA counts the reads for both of these regions.
    // But if -one-hit set, these reads will be filtered.
    if (args.one_hit == 1 && n_gene >1) {
        free(str.s);
        free(s);
        return 1;
    }
    
    int i;
    for (i = 0; i < n_gene; ++i) {
        // Features (Gene or Region)
        char *val = str.s + s[i];
        
        int idx = dict_query(args.features, val);
        if (idx == -1) idx = dict_push(args.features, val);

        struct PISA_dna_pool *v = dict_query_value(args.features, idx);

        if (v == NULL) {
            v = PISA_dna_pool_init();
            dict_assign_value(args.features, idx, v);
        } 
        // not store cell barcode for each hash, use id number instead to reduce memory
        struct PISA_dna *c= PISA_idx_query(v, cell_id);
        if (c == NULL) {
            c = PISA_idx_push(v, cell_id);
        //if (c->data == NULL) {
            struct counts *counts = malloc(sizeof(struct counts));
            if (args.umi_tag)  {
                counts->p = PISA_dna_pool_init();
                counts->up = args.velocity == 1 ? PISA_dna_pool_init() : NULL;
            }
            else {
                counts->count = 0;
                counts->unspliced = 0;
            }
            c->data = counts;
        }

        if (args.umi_tag) {
            uint8_t *umi_tag = bam_aux_get(b, args.umi_tag);
            assert(umi_tag);
            char *val = (char*)(umi_tag+1);
            assert(c->data);
            struct counts *count = c->data;
            PISA_dna_push(count->p, val);

            if (args.velocity && unspliced)
                PISA_dna_push(count->up, val);
        }
        else {
            struct counts *count = c->data;
            count->count++;

            if (args.velocity && unspliced)
                count->unspliced++;
        }
    }
    free(str.s);
    free(s);
    return 0;
}

static void update_counts()
{
    int n_feature = dict_size(args.features);
    int i;
    for (i = 0; i < n_feature; ++i) {
        struct PISA_dna_pool *v = dict_query_value(args.features, i);
        int j;
        int n_cell = v->l;
        for (j = 0; j < n_cell; ++j) {
            struct counts *count = v->data[j].data;
            assert(count);
            if (args.umi_tag) {
                int size = count->p->l;
                PISA_dna_destroy(count->p);
                count->count = size;
            }

            if (args.velocity) {
                if (args.umi_tag) {
                    int size = count->up->l;
                    PISA_dna_destroy(count->up);
                    count->unspliced = size;
                }
            }
            args.n_record += count->count;
            args.n_record2 += count->unspliced;
        }
    }
}
static void write_outs()
{
    int n_barcode = dict_size(args.barcodes);
    int n_feature = dict_size(args.features);

    if (n_barcode == 0) error("No barcode found.");
    if (n_feature == 0) error("No feature found.");
    if (args.n_record == 0) {
        warnings("No anntated record found.");
        return;
    }
    args.n_record1 = args.n_record;

    if (args.velocity) args.n_record1 = args.n_record - args.n_record2;
    
    if (args.outdir) {
        kstring_t barcode_str = {0,0,0};
        kstring_t feature_str = {0,0,0};
        kstring_t mex_str = {0,0,0};
        kstring_t unspliced_str = {0,0,0};
        
        kputs(args.outdir, &barcode_str);
        kputs(args.outdir, &feature_str);
        kputs(args.outdir, &mex_str);
        kputs(args.outdir, &unspliced_str);

        if (args.outdir[strlen(args.outdir)-1] != '/') {
            kputc('/', &barcode_str);
            kputc('/', &feature_str);
            kputc('/', &mex_str);
            kputc('/', &unspliced_str);
        }

        if (args.prefix) {
            kputs(args.prefix, &barcode_str);
            kputs(args.prefix, &feature_str);
            kputs(args.prefix, &mex_str);
            kputs(args.prefix, &unspliced_str);
        }
        
        kputs("barcodes.tsv.gz", &barcode_str);
        kputs("features.tsv.gz", &feature_str);
        if (args.velocity)
            kputs("spliced.mtx.gz", &mex_str);
        else
            kputs("matrix.mtx.gz", &mex_str);

        kputs("unspliced.mtx.gz", &unspliced_str);
        
        BGZF *barcode_fp = bgzf_open(barcode_str.s, "w");
        bgzf_mt(barcode_fp, args.n_thread, 256);
        CHECK_EMPTY(barcode_fp, "%s : %s.", barcode_str.s, strerror(errno));
        
        int i;

        kstring_t str = {0,0,0};
        kstring_t str2 = {0,0,0};
        
        for (i = 0; i < n_barcode; ++i) {
            kputs(dict_name(args.barcodes, i), &str);
            kputc('\n', &str);
        }
        int l = bgzf_write(barcode_fp, str.s, str.l);
        if (l != str.l) error("Failed to write.");
        bgzf_close(barcode_fp);

        str.l = 0;
        BGZF *feature_fp = bgzf_open(feature_str.s, "w");
        bgzf_mt(feature_fp, args.n_thread, 256);
        CHECK_EMPTY(feature_fp, "%s : %s.", feature_str.s, strerror(errno));
        for (i = 0; i < n_feature; ++i) {
            kputs(dict_name(args.features,i), &str);
            kputc('\n', &str);
        }
        l = bgzf_write(feature_fp, str.s, str.l);
        if (l != str.l) error("Failed to write.");
        
        bgzf_close(feature_fp);

        str.l = 0;

        BGZF *mex_fp = bgzf_open(mex_str.s, "w");
        CHECK_EMPTY(mex_fp, "%s : %s.", mex_str.s, strerror(errno));
        
        bgzf_mt(mex_fp, args.n_thread, 256);
        kputs("%%MatrixMarket matrix coordinate integer general\n", &str);
        kputs("% Generated by PISA ", &str);
        kputs(PISA_VERSION, &str);
        kputc('\n', &str);
        ksprintf(&str, "%d\t%d\t%llu\n", n_feature, n_barcode, args.n_record1);

        BGZF *unspliced_fp = NULL;
        if (args.velocity) {
            unspliced_fp = bgzf_open(unspliced_str.s, "w");
            if (unspliced_fp == NULL) error("%s : %s.", unspliced_str.s, strerror(errno));
        
            bgzf_mt(unspliced_fp, args.n_thread, 256);
            kputs("%%MatrixMarket matrix coordinate integer general\n", &str2);
            kputs("% Generated by PISA ", &str2);
            kputs(PISA_VERSION, &str2);
            kputc('\n', &str2);
            ksprintf(&str2, "%d\t%d\t%llu\n", n_feature, n_barcode, args.n_record2);
        }
        
        for (i = 0; i < n_feature; ++i) {
            struct PISA_dna_pool *v = dict_query_value(args.features, i);
            int j;
            int n_cell = v->l;
            for (j = 0; j < n_cell; ++j) {
                struct counts *count = v->data[j].data;
                if (args.velocity) {
                    int spliced = count->count - count->unspliced;
                    if (spliced > 0) ksprintf(&str, "%d\t%d\t%u\n", i+1, v->data[j].idx+1, spliced);
                    if (count->unspliced > 0) ksprintf(&str2, "%d\t%d\t%u\n", i+1, v->data[j].idx+1, count->unspliced);
                }
                else
                    ksprintf(&str, "%d\t%d\t%u\n", i+1, v->data[j].idx+1, count->count);
                
                free(count);
            }

            if (str.l > 100000000) {
                int l = bgzf_write(mex_fp, str.s, str.l);
                if (l != str.l) error("Failed to write file.");
                str.l = 0;

                if (args.velocity) {
                    l = bgzf_write(unspliced_fp, str2.s, str2.l);
                    if (l != str2.l) error("Failed to write file.");
                    str2.l = 0;                
                }
            }
        }

        if (str.l) {
            l = bgzf_write(mex_fp, str.s, str.l);
            if (l != str.l) error("Failed to wirte.");
        }

        if (str2.l) {
            l = bgzf_write(unspliced_fp, str2.s, str2.l);
            if (l != str2.l) error("Failed to wirte.");
        
        }

        free(str.s);
        if (str2.m) free(str2.s);
        free(mex_str.s);
        free(barcode_str.s);
        free(feature_str.s);
        if (unspliced_str.m) free(unspliced_str.s);
        bgzf_close(mex_fp);
    }
    
    // header
    if (args.output_fname) {
        int i;
        FILE *out = fopen(args.output_fname, "w");
        CHECK_EMPTY(out, "%s : %s.", args.output_fname, strerror(errno));
        fputs("ID", out);
        
        for (i = 0; i < n_barcode; ++i)
            fprintf(out, "\t%s", dict_name(args.barcodes, i));
        fprintf(out, "\n");
        uint32_t *temp = malloc(n_barcode*sizeof(int));        
        for (i = 0; i < n_feature; ++i) {
            struct PISA_dna_pool *v = dict_query_value(args.features, i);
            int j;
            int n_cell = v->l;
            memset(temp, 0, sizeof(int)*n_barcode);
            fputs(dict_name(args.features, i), out);
            for (j = 0; j < n_cell; ++j) {
                int idx = v->data[j].idx;
                temp[idx] = v->data[j].count;
            }

            for (j = 0; j < n_barcode; ++j)
                fprintf(out, "\t%u", temp[j]);
            fputc('\n', out);
        }
        fclose(out);
        free(temp);
    }

}

int count_matrix(int argc, char **argv)
{
    double t_real;
    t_real = realtime();
    if (parse_args(argc, argv)) return bam_count_usage();
        
    bam1_t *b;

    int ret;
    b = bam_init1();
    int region_type_flag = 0;
    
    for (;;) {
        //ret = sam_read1(args.fp_in, args.hdr, b);
        ret = read_bam_files(args.files, b);
        if (ret < 0) break;
        bam_hdr_t *hdr = get_hdr(args.files);

        char *alias = get_alias(args.files);
        if (args.alias_file_cb == 1 && !alias)
            error("No alias found for %s", get_fname(args.files));
        
        bam1_core_t *c;
        c = &b->core;

        if (c->tid <= -1 || c->tid > hdr->n_targets || (c->flag & BAM_FUNMAP)) continue;
        if (c->qual < args.mapq_thres) continue;
        if (args.use_dup == 0 && c->flag & BAM_FDUP) continue;
        
        if (args.n_type > 0) {

            uint8_t *data = bam_aux_get(b, args.region_type_tag);
            if (!data) continue;

            region_type_flag = 0;
            int k;
            for (k = 0; k < args.n_type; ++k) {
                if (args.region_types[k] == RE_type_map(data[1])) {
                    region_type_flag = 1;
                    break;
                }
            }

            if (region_type_flag == 0) continue;
        }
        
        count_matrix_core(b, alias);
    }
    
    bam_destroy1(b);
    
    if (ret != -1) warnings("Truncated file?");   

    update_counts();

    write_outs();
    
    memory_release();
    
    LOG_print("Real time: %.3f sec; CPU: %.3f sec; Peak RSS: %.3f GB.", realtime() - t_real, cputime(), peakrss() / 1024.0 / 1024.0 / 1024.0);
    return 0;
}
