/* main.c - small driver for loader.c.
 *
 * Usage: main [csv_file]
 * Without an argument it loads the PVGIS dataset in datasets/.
 */
#include "../common.h"

#define DEFAULT_CSV "datasets/Timeseries_5.700_101.840_E5_10deg_0deg_2019_2019.csv"

static void print_record(const char *label, const HourRecord *r)
{
    printf("  %-6s %04d-%02d-%02d %02d:00  G=%8.2f W/m2  T=%5.2f C  valid=%d\n",
           label, r->year, r->month, r->day, r->hour,
           r->irradiance, r->temp_c, r->valid);
}

static void print_summary(const Dataset *ds)
{
    int i, n_valid = 0;
    double sum_g = 0.0, sum_t = 0.0, max_g = 0.0;

    for (i = 0; i < ds->n; i++) {
        const HourRecord *r = &ds->rec[i];
        if (!r->valid)
            continue;
        n_valid++;
        sum_g += r->irradiance;
        sum_t += r->temp_c;
        if (r->irradiance > max_g)
            max_g = r->irradiance;
    }

    printf("Rows kept     : %d\n", ds->n);
    printf("Rows dropped  : %d\n", ds->n_dropped);
    printf("Rows valid    : %d\n", n_valid);
    printf("Rows invalid  : %d\n", ds->n - n_valid);
    if (n_valid > 0) {
        printf("Mean irradiance: %.2f W/m2 (max %.2f)\n", sum_g / n_valid, max_g);
        printf("Mean temp      : %.2f C\n", sum_t / n_valid);
        printf("Energy (sum G) : %.2f kWh/m2\n", sum_g / 1000.0);
    }
    if (ds->n > 0) {
        print_record("first", &ds->rec[0]);
        print_record("last", &ds->rec[ds->n - 1]);
    }
}

int main(int argc, char *argv[])
{
    const char *path;
    Dataset ds;

    if (argc > 1)
        path = argv[1];
    else
        path = DEFAULT_CSV;

    printf("Loading '%s'\n", path);
    if (load_csv(path, &ds) != 0) {
        fprintf(stderr, "main: load_csv failed\n");
        return (1);
    }

    print_summary(&ds);
    free_dataset(&ds);
    return (0);
}
