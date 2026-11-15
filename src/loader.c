/* M1 loader.c - CSV loading for hourly irradiance/temperature data.
 *
 * Accepted layout (PVGIS style and plain CSVs both work):
 *   - optional UTF-8 byte order mark at the start of the file
 *   - optional preamble lines BEFORE the data that contain no comma
 *     (e.g. "Latitude (decimal degrees):<TAB>5.700") are skipped silently
 *   - optional header row whose first field is time/timestamp/date/datetime;
 *     columns named G(i)/G(h)/GHI/irradiance and T2m/temp are located by name,
 *     otherwise column 2 = irradiance and column 3 = temperature
 *   - timestamps: YYYYMMDD:HHMM, YYYY-MM-DD HH:MM, YYYY-MM-DDTHH:MM, YYYY/MM/DD HH:MM
 *   - LF or CRLF line endings, blank lines, trailing commas
 *
 * Row policy:
 *   - timestamp unparseable or not a real calendar date -> row dropped,
 *     line number printed to stderr
 *   - line longer than LINE_BUF - 1 characters -> row dropped (never kept truncated)
 *   - timestamp ok, value blank/NaN/non-numeric -> row kept with valid = 0
 *
 * load_csv() returns 0 on success and -1 on any failure. On every call, ds is
 * reset first, so free_dataset() is always safe afterwards.
 */
#include "../common.h"

#define LINE_BUF   1024
#define MAX_COLS   32
#define INIT_CAP   8800          /* one year of hourly rows */

/* Progress of one load, shared by the helpers below. */
typedef struct {
    int cap;                     /* allocated rows in ds->rec */
    int line_no;                 /* physical line currently being read */
    int n_valid;                 /* rows kept with valid == 1 */
    int any_content;             /* saw at least one non-blank line */
    int seen_data;               /* saw at least one good timestamp */
    int c_irr, c_tmp;            /* column index of irradiance / temperature */
} LoadState;

/* ---- reading ---------------------------------------------------------- */

#define READ_EOF       0
#define READ_OK        1
#define READ_TOO_LONG  2

/* Read one line into buf. A line that does not fit is reported as
 * READ_TOO_LONG and the rest of it is discarded, so it is neither parsed
 * truncated nor mistaken for a new row. Sentinels in the last two bytes tell
 * whether fgets filled the whole buffer; this stays correct even if the line
 * contains a NUL byte (where strlen/strchr would be fooled). */

static void discard_rest_of_line(FILE *fp)
{
    int ch;

    ch = fgetc(fp);
    while (ch != '\n' && ch != EOF)
        ch = fgetc(fp);
}

static int read_line(FILE *fp, char *buf, int size)
{
    buf[size - 2] = 1;
    buf[size - 1] = 1;
    if (!fgets(buf, size, fp))
        return (READ_EOF);
    if (buf[size - 1] == 0 && buf[size - 2] != '\n')
    {
        discard_rest_of_line(fp);
        return (READ_TOO_LONG);
    }
    return (READ_OK);
}

/* Skip a UTF-8 byte order mark at the start of the file. */
static char *skip_bom(char *line)
{
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF)
        return (line + 3);
    return (line);
}

/* ---- classifying a line ----------------------------------------------- */

/* Before the first data row, text lines and the header row are not data.
 * Returns 1 if the line was consumed (preamble or header), 0 otherwise. */
static int consume_preamble_or_header(char *f[], int nf, int has_comma, LoadState *st)
{
    if (st->seen_data)
        return (0);
    if (!has_comma)
        return (1);                                   /* preamble text */
    if (header_like(f[0]))
    {
        map_columns(f, nf, &st->c_irr, &st->c_tmp); /* header row */
        return (1);
    }
    return (0);
}

static void drop_line(Dataset *ds, const LoadState *st, const char *why)
{
    fprintf(stderr, "load_csv: line %d dropped (%s)\n", st->line_no, why);
    ds->n_dropped++;
}

/* ---- building a record ------------------------------------------------ */

/* Fill the irradiance/temperature of r; valid = 0 if either is unusable. */
static void read_values(HourRecord *r, char *f[], int nf, const LoadState *st)
{
    r->valid = 1;
    r->irradiance = 0.0;
    r->temp_c = 0.0;
    if (st->c_irr >= nf || parse_value(f[st->c_irr], &r->irradiance) != 0)
        r->valid = 0;
    if (st->c_tmp >= nf || parse_value(f[st->c_tmp], &r->temp_c) != 0)
        r->valid = 0;
    if (!r->valid)
    {
        r->irradiance = 0.0;
        r->temp_c = 0.0;
    }
}

/* Append r to ds, growing the array when full. 0 ok, -1 out of memory. */
static int append_record(Dataset *ds, LoadState *st, const HourRecord *r)
{
    if (ds->n == st->cap)
    {
        int newcap;
        HourRecord *tmp;

        if (st->cap == 0)
            newcap = INIT_CAP;      /* first allocation */
        else
            newcap = st->cap * 2;   /* double when full */
        tmp = realloc(ds->rec, (size_t)newcap * sizeof *tmp);
        if (!tmp)
            return (-1);
        ds->rec = tmp;
        st->cap = newcap;
    }
    ds->rec[ds->n++] = *r;
    if (r->valid)
        st->n_valid++;
    return (0);
}

/* ---- one line, start to finish ---------------------------------------- */

/* 0 = handled (kept, dropped or skipped), -1 = fatal (out of memory). */
static int process_line(Dataset *ds, LoadState *st, char *line)
{
    char *s = trim(line);                  /* removes CR, LF, spaces */
    char *f[MAX_COLS];
    HourRecord r;
    int nf, has_comma;

    if (*s == '\0')
        return (0);                          /* blank line */
    st->any_content = 1;
    has_comma = strchr(s, ',') != NULL;
    nf = split_fields(s, f, MAX_COLS);
    f[0] = trim(f[0]);
    if (parse_timestamp(f[0], &r) != 0)
    {
        if (!consume_preamble_or_header(f, nf, has_comma, st))
            drop_line(ds, st, "bad timestamp");
        return (0);
    }
    st->seen_data = 1;
    read_values(&r, f, nf, st);
    return (append_record(ds, st, &r));
}

/* ---- wrap-up ---------------------------------------------------------- */

/* Report problems with the finished load. 0 ok, -1 failure (ds is freed). */
static int finish_load(const char *path, Dataset *ds, const LoadState *st)
{
    if (ds->n_dropped > 0)
        fprintf(stderr, "load_csv: dropped %d unusable row(s)\n", ds->n_dropped);
    if (!st->any_content)
    {
        fprintf(stderr, "load_csv: '%s' is empty\n", path);
        free_dataset(ds);
        return (-1);
    }
    if (ds->n == 0)
    {
        fprintf(stderr, "load_csv: '%s' has a header but no usable data rows\n", path);
        free_dataset(ds);
        return (-1);
    }
    if (st->n_valid < MIN_RECORDS)
        fprintf(stderr, "load_csv: warning: only %d valid row(s), fewer than MIN_RECORDS (%d)\n", st->n_valid, MIN_RECORDS);
    return (0);
}

/* ---- public API ------------------------------------------------------- */

void free_dataset(Dataset *ds)
{
    if (!ds)
        return;
    free(ds->rec);
    ds->rec = NULL;
    ds->n = 0;
    ds->n_dropped = 0;
}

int load_csv(const char *path, Dataset *ds)
{
    FILE *fp;
    char line[LINE_BUF];
    LoadState st = {0};
    char *row;
    int status;

    if (!ds)
        return (-1);
    ds->rec = NULL;                 /* always leave ds in a freeable state */
    ds->n = 0;
    ds->n_dropped = 0;
    if (!path)
        return (-1);
    st.c_irr = 1;
    st.c_tmp = 2;
    fp = fopen(path, "r");
    if (!fp)
    {
        fprintf(stderr, "load_csv: cannot open '%s'\n", path);
        return (-1);
    }
    while ((status = read_line(fp, line, sizeof line)) != READ_EOF)
    {
        st.line_no++;
        if (status == READ_TOO_LONG)
        {
            st.any_content = 1;
            drop_line(ds, &st, "line too long");
            continue;
        }
        row = line;
        if (st.line_no == 1)
            row = skip_bom(line);       /* a BOM can only start the file */
        if (process_line(ds, &st, row) != 0)
        {
            fprintf(stderr, "load_csv: out of memory\n");
            fclose(fp);
            free_dataset(ds);
            return (-1);
        }
    }
    fclose(fp);
    return (finish_load(path, ds, &st));
}
