#ifndef KLIOS716_FTS3519_H
#define KLIOS716_FTS3519_H

#define FTS3519_MAX_POINTS 10u

struct fts3519_point {
    unsigned int x;
    unsigned int y;
    unsigned int raw_x;
    unsigned int raw_y;
    unsigned char id;
    unsigned char down;
    unsigned char b[6];
};

/* Initialise SPI1, power/reset FTS3519 and verify its chip ID. */
int fts3519_init(void);

/* Return the number of parsed points, zero for no touch, or a negative error. */
int fts3519_read_points(struct fts3519_point *points, unsigned int capacity);

#endif
