/* test_loader.c - tests for load_csv() built on the real dataset in datasets/.
 *
 * Each test derives a small file from the real PVGIS CSV (truncate, corrupt,
 * convert line endings, ...) into tests/tmp/, loads it, checks the result and
 * deletes it again. Run from the project root:
 *
 *   gcc -Wall -Wextra -I. src/loader.c src/loader_helper.c tests/test_loader.c -o test_loader -lm
 *   ./test_loader
 */
#include "../common.h"

#define DATASET  "datasets/Timeseries_5.700_101.840_E5_10deg_0deg_2019_2019.csv"
#define TMP_DIR  "tests"
#define TMP_FILE "tests/tmp_case.csv"
#define MAX_LINE 1024

static int g_run = 0, g_failed = 0;

#define CHECK(cond) do {                                                   \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            printf("    FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                                  \
    } while (0)

/* ---- helpers to build test files from the real dataset ---------------- */

/* Copy lines [from, to] (1-based, inclusive) of the dataset to out. */
static int copy_lines(FILE *out, int from, int to)
{
    char line[MAX_LINE];
    int n = 0;
    FILE *in = fopen(DATASET, "rb");

    if (!in)
        return (-1);
    while (fgets(line, sizeof line, in)) {
        n++;
        if (n >= from && n <= to)
            fputs(line, out);
        if (n > to)
            break;
    }
    fclose(in);
    return (0);
}

static FILE *open_tmp(void)
{
    return (fopen(TMP_FILE, "wb"));   /* binary: no 
 -> 
 on Windows */
}

/* Hand-written rows follow the PVGIS header: time,G(i),H_sun,T2m,WS10m,Int */
/* Dataset layout: lines 1-8 preamble, line 9 header, 10-8769 data. */
#define HEADER_LINE 9
#define FIRST_DATA  10

static int count_cr(const char *path)
{
    int ch, n = 0;
    FILE *f = fopen(path, "rb");

    if (!f)
        return (-1);
    while ((ch = fgetc(f)) != EOF)
        n += (ch == 13);                 /* carriage return */
    fclose(f);
    return (n);
}

static void report(const char *name, int failed_before)
{
    if (g_failed == failed_before)
        printf("  [ok  ] %s\n", name);
    else
        printf("  [FAIL] %s\n", name);
}

/* ---- tests ------------------------------------------------------------ */

static void test_full_dataset(void)
{
    Dataset ds;
    int before = g_failed, i, bad = 0;

    CHECK(load_csv(DATASET, &ds) == 0);
    CHECK(ds.n == 8760);                       /* one year, hourly */
    for (i = 0; i < ds.n; i++)
        bad += !ds.rec[i].valid;
    CHECK(bad == 0);
    CHECK(ds.rec[0].year == 2019 && ds.rec[0].month == 1 &&
          ds.rec[0].day == 1 && ds.rec[0].hour == 0);
    CHECK(ds.rec[0].irradiance > 162.12 && ds.rec[0].irradiance < 162.14);
    CHECK(ds.rec[0].temp_c > 21.36 && ds.rec[0].temp_c < 21.38);
    CHECK(ds.rec[ds.n - 1].month == 12 && ds.rec[ds.n - 1].day == 31);
    free_dataset(&ds);
    CHECK(ds.rec == NULL && ds.n == 0);
    report("full real dataset", before);
}

static void test_preamble_and_header_skipped(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 59);         /* preamble + header + 60 rows */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60);
    CHECK(ds.n_dropped == 0);
    free_dataset(&ds);
    report("preamble + header skipped, 60 rows", before);
}

static void test_no_header(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, FIRST_DATA, FIRST_DATA + 59); /* data only */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60 && ds.n_dropped == 0);
    CHECK(ds.rec[0].valid == 1);                /* falls back to default columns */
    free_dataset(&ds);
    report("no header (default columns)", before);
}

static void test_lf_line_endings(void)
{
    Dataset ds;
    char line[MAX_LINE];
    FILE *in = fopen(DATASET, "rb"), *f = open_tmp();
    int n = 0, before = g_failed;

    while (fgets(line, sizeof line, in) && n < FIRST_DATA + 59) {
        size_t len = strlen(line);
        n++;
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
            line[--len] = '\0';
        fprintf(f, "%s\n", line);               /* LF only */
    }
    fclose(in);
    fclose(f);
    CHECK(count_cr(TMP_FILE) == 0);             /* file really is LF only */
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60 && ds.n_dropped == 0);
    CHECK(ds.rec[59].valid == 1);
    free_dataset(&ds);
    report("LF line endings (CRLF in the original)", before);
}

static void test_bad_timestamps_dropped(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 59);
    fputs("not-a-time,100,10,20,1,0\r\n", f);
    fputs("20191301:0030,100,10,20,1,0\r\n", f);  /* month 13 */
    fputs("20190101:2530,100,10,20,1,0\r\n", f);  /* hour 25  */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60);
    CHECK(ds.n_dropped == 3);
    free_dataset(&ds);
    report("bad timestamps dropped and counted", before);
}

static void test_bad_values_kept_invalid(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 59);
    fputs("20190103:0030,,10,20,1,0\r\n", f);      /* blank irradiance  */
    fputs("20190103:0130,NaN,10,20,1,0\r\n", f);   /* NaN               */
    fputs("20190103:0230,abc,10,20,1,0\r\n", f);   /* non-numeric       */
    fputs("20190103:0330,100,10,,1,0\r\n", f);     /* blank temperature */
    fputs("20190103:0430,100,10,20x,1,0\r\n", f);  /* junk temperature  */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 65);
    CHECK(ds.n_dropped == 0);
    CHECK(ds.rec[59].valid == 1);
    CHECK(ds.rec[60].valid == 0 && ds.rec[61].valid == 0 && ds.rec[62].valid == 0);
    CHECK(ds.rec[63].valid == 0 && ds.rec[64].valid == 0);
    CHECK(ds.rec[60].irradiance == 0.0);
    free_dataset(&ds);
    report("blank/NaN/non-numeric kept with valid = 0", before);
}

static void test_blank_lines_and_trailing_commas(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 59);
    fputs("\r\n\r\n", f);
    fputs("20190103:0030,200.5,10,25.5,\r\n", f);    /* trailing comma */
    fputs("20190103:0130,210.5,10,25.0,,,\r\n", f);  /* many commas    */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 62 && ds.n_dropped == 0);
    CHECK(ds.rec[60].valid == 1 && ds.rec[61].valid == 1);
    CHECK(ds.rec[60].irradiance > 200.4 && ds.rec[60].irradiance < 200.6);
    free_dataset(&ds);
    report("blank lines and trailing commas", before);
}

static void test_footer_dropped(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, 8800);                      /* whole file incl. footer */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 8760);
    CHECK(ds.n_dropped > 0);                     /* the PVGIS text footer */
    free_dataset(&ds);
    report("PVGIS footer text counted as dropped", before);
}

static void test_too_few_records_warns_but_loads(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 9);            /* 10 rows < MIN_RECORDS */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);         /* warning only */
    CHECK(ds.n == 10);
    free_dataset(&ds);
    report("fewer than MIN_RECORDS: warns, still loads", before);
}

static void test_header_only(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, HEADER_LINE);               /* preamble + header only */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == -1);
    CHECK(ds.rec == NULL && ds.n == 0);
    report("header only returns -1", before);
}

static void test_empty_file(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    fclose(f);                                   /* 0 bytes */
    CHECK(load_csv(TMP_FILE, &ds) == -1);
    CHECK(ds.rec == NULL && ds.n == 0);
    report("empty file returns -1", before);
}

static void test_missing_file_and_bad_args(void)
{
    Dataset ds;
    int before = g_failed;

    CHECK(load_csv("datasets/does_not_exist.csv", &ds) == -1);
    CHECK(load_csv(NULL, &ds) == -1);
    CHECK(load_csv(DATASET, NULL) == -1);
    report("missing file / NULL args return -1", before);
}

/* ---- edge cases / hidden-bug hunt ------------------------------------- */

static void test_null_path_resets_dataset(void)
{
    Dataset ds;
    int before = g_failed, rc;

    ds.rec = (HourRecord *)1;                    /* garbage the caller left */
    ds.n = 5;
    ds.n_dropped = 2;
    rc = load_csv(NULL, &ds);
    CHECK(rc == -1);
    CHECK(ds.rec == NULL && ds.n == 0 && ds.n_dropped == 0);
    report("NULL path still resets ds (safe to free_dataset)", before);
}

static void test_embedded_nul_does_not_eat_next_row(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;
    static const char bad[] = "20190103:0030,100,10,20,1,0\0junk\r\n";

    copy_lines(f, 1, FIRST_DATA + 9);            /* 10 good rows */
    fwrite(bad, 1, sizeof bad - 1, f);           /* row with a NUL byte */
    fputs("20190103:0130,110,10,21,1,0\r\n", f); /* must survive */
    fputs("20190103:0230,120,10,22,1,0\r\n", f); /* must survive */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 13);                           /* 10 + NUL row + 2 */
    free_dataset(&ds);
    report("embedded NUL byte does not swallow the next row", before);
}

static void test_overlong_line_dropped(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed, i;

    copy_lines(f, 1, FIRST_DATA + 59);           /* header + 60 rows */
    fputs("20190103:0030,", f);
    for (i = 0; i < 1005; i++)
        fputc(' ', f);
    fputs("162.13,10,25.0,1,0\r\n", f);          /* > 1023 chars: truncated */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60);                           /* truncated row not kept */
    CHECK(ds.n_dropped == 1);
    free_dataset(&ds);
    report("over-long line is dropped, not kept truncated", before);
}

static void test_utf8_bom_headerless(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    fputs("\xEF\xBB\xBF", f);                    /* UTF-8 BOM */
    copy_lines(f, FIRST_DATA, FIRST_DATA + 59);  /* 60 rows, no header */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 60);
    CHECK(ds.n_dropped == 0);
    free_dataset(&ds);
    report("UTF-8 BOM does not lose the first row", before);
}

static void test_impossible_dates_dropped(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 59);
    fputs("20190230:0030,100,10,20,1,0\r\n", f); /* 30 Feb            */
    fputs("20190431:0030,100,10,20,1,0\r\n", f); /* 31 Apr            */
    fputs("20190229:0030,100,10,20,1,0\r\n", f); /* 2019 not leap     */
    fputs("20200229:0030,100,10,20,1,0\r\n", f); /* 2020 leap: valid  */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 61);                           /* 60 + the leap day */
    CHECK(ds.n_dropped == 3);
    free_dataset(&ds);
    report("impossible calendar dates dropped, leap day kept", before);
}

static void test_realloc_growth(void)
{
    Dataset ds;
    FILE *f = open_tmp();
    int before = g_failed;

    copy_lines(f, 1, FIRST_DATA + 8759);         /* preamble+header+8760 rows */
    copy_lines(f, FIRST_DATA, FIRST_DATA + 8759);/* 8760 more: 17520 total   */
    fclose(f);
    CHECK(load_csv(TMP_FILE, &ds) == 0);
    CHECK(ds.n == 17520);                        /* crosses INIT_CAP = 8800 */
    if (ds.n == 17520) {
        CHECK(ds.rec[8799].irradiance == ds.rec[39].irradiance);
        CHECK(ds.rec[8800].irradiance == ds.rec[40].irradiance);
        CHECK(ds.rec[17519].hour == ds.rec[8759].hour);
    }
    free_dataset(&ds);
    report("realloc growth past INIT_CAP keeps data intact", before);
}

/* ---- runner ----------------------------------------------------------- */

int main(void)
{
    FILE *probe = fopen(DATASET, "r");

    if (!probe) {
        printf("Cannot find %s - run from the project root.\n", DATASET);
        return (2);
    }
    fclose(probe);
    (void)TMP_DIR;

    printf("load_csv tests\n");
    test_full_dataset();
    test_preamble_and_header_skipped();
    test_no_header();
    test_lf_line_endings();
    test_bad_timestamps_dropped();
    test_bad_values_kept_invalid();
    test_blank_lines_and_trailing_commas();
    test_footer_dropped();
    test_too_few_records_warns_but_loads();
    test_header_only();
    test_empty_file();
    test_missing_file_and_bad_args();

    printf("\nedge cases\n");
    test_null_path_resets_dataset();
    test_embedded_nul_does_not_eat_next_row();
    test_overlong_line_dropped();
    test_utf8_bom_headerless();
    test_impossible_dates_dropped();
    test_realloc_growth();

    remove(TMP_FILE);
    printf("\n%d checks, %d failed\n", g_run, g_failed);
    if (g_failed == 0)
        return (0);
    return (1);
}
