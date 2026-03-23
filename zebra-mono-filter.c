/*
 * CUPS filter for Zebra ZD621: crisp 1-bit monochrome label printing.
 *
 * Renders PDF using CoreGraphics with anti-aliasing fully disabled,
 * thresholds to 1-bit, and outputs ZPL ^GFA directly to the printer.
 *
 * Uses only macOS system frameworks — no external dependencies.
 * Compiles with: cc -framework CoreGraphics -framework CoreFoundation -o zebra-mono-filter zebra-mono-filter.c
 *
 * CUPS filter interface: filter job-id user title copies options [file]
 */

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DPI 203
#define LABEL_W_IN 4.0
#define LABEL_H_IN 6.0
#define PIXEL_W ((int)(LABEL_W_IN * DPI))  /* 812 */
#define PIXEL_H ((int)(LABEL_H_IN * DPI))  /* 1218 */
#define THRESHOLD 128

static char *read_stdin_to_tmpfile(void) {
    char template[] = "/private/var/spool/cups/tmp/zebra_XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        /* Fallback */
        strcpy(template, "/tmp/zebra_XXXXXX");
        fd = mkstemp(template);
        if (fd < 0) {
            fprintf(stderr, "ERROR: Cannot create temp file\n");
            return NULL;
        }
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(fd, buf, n);
    }
    close(fd);

    char *path = strdup(template);
    return path;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Usage: zebra-mono-filter job-id user title copies options [file]\n");
        return 1;
    }

    const char *input_path = NULL;
    char *tmp_path = NULL;

    if (argc >= 7) {
        input_path = argv[6];
    } else {
        tmp_path = read_stdin_to_tmpfile();
        if (!tmp_path) return 1;
        input_path = tmp_path;
    }

    /* Open PDF document */
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, (const UInt8 *)input_path, strlen(input_path), false
    );
    if (!url) {
        fprintf(stderr, "ERROR: Cannot create URL for %s\n", input_path);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!doc) {
        fprintf(stderr, "ERROR: Cannot open PDF %s\n", input_path);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    CGPDFPageRef page = CGPDFDocumentGetPage(doc, 1);
    if (!page) {
        fprintf(stderr, "ERROR: PDF has no pages\n");
        CGPDFDocumentRelease(doc);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    CGRect mediaBox = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    double pdf_w = mediaBox.size.width;
    double pdf_h = mediaBox.size.height;

    /* Create 8-bit grayscale bitmap context */
    CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
    size_t bpr = PIXEL_W; /* 1 byte per pixel */
    unsigned char *buffer = (unsigned char *)calloc(PIXEL_H, bpr);
    if (!buffer) {
        fprintf(stderr, "ERROR: Cannot allocate bitmap buffer\n");
        CGColorSpaceRelease(colorspace);
        CGPDFDocumentRelease(doc);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    CGContextRef ctx = CGBitmapContextCreate(
        buffer, PIXEL_W, PIXEL_H, 8, bpr, colorspace, kCGImageAlphaNone
    );
    CGColorSpaceRelease(colorspace);

    if (!ctx) {
        fprintf(stderr, "ERROR: Cannot create bitmap context\n");
        free(buffer);
        CGPDFDocumentRelease(doc);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    /* Disable ALL anti-aliasing, smoothing, and interpolation */
    CGContextSetAllowsAntialiasing(ctx, false);
    CGContextSetShouldAntialias(ctx, false);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextSetAllowsFontSmoothing(ctx, false);
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetAllowsFontSubpixelQuantization(ctx, false);
    CGContextSetAllowsFontSubpixelPositioning(ctx, false);

    /* Fill background with white */
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, PIXEL_W, PIXEL_H));

    /* Scale PDF to fit label, maintaining aspect ratio */
    double scale_x = (double)PIXEL_W / pdf_w;
    double scale_y = (double)PIXEL_H / pdf_h;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;

    /* Center on label */
    double offset_x = ((double)PIXEL_W - pdf_w * scale) / 2.0;
    double offset_y = ((double)PIXEL_H - pdf_h * scale) / 2.0;

    CGContextTranslateCTM(ctx, offset_x, offset_y);
    CGContextScaleCTM(ctx, scale, scale);

    /* Draw the PDF page */
    CGContextDrawPDFPage(ctx, page);

    /* Convert 8-bit grayscale to ZPL ^GFA (1-bit, 1=black) */
    int zpl_bpr = (PIXEL_W + 7) / 8;  /* bytes per row in ZPL */
    int total_bytes = zpl_bpr * PIXEL_H;

    /* Allocate hex output buffer: 2 hex chars per byte + some overhead */
    size_t hex_size = (size_t)total_bytes * 2 + 1;
    char *hex_buf = (char *)malloc(hex_size);
    if (!hex_buf) {
        fprintf(stderr, "ERROR: Cannot allocate hex buffer\n");
        CGContextRelease(ctx);
        free(buffer);
        CGPDFDocumentRelease(doc);
        if (tmp_path) { unlink(tmp_path); free(tmp_path); }
        return 1;
    }

    /* CG bitmap data in memory is top-to-bottom, matching ZPL row order. */
    char *hex_ptr = hex_buf;
    for (int y = 0; y < PIXEL_H; y++) {
        for (int byte_x = 0; byte_x < zpl_bpr; byte_x++) {
            unsigned char zpl_byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int x = byte_x * 8 + bit;
                if (x < PIXEL_W) {
                    unsigned char gray = buffer[y * bpr + x];
                    if (gray < THRESHOLD) {
                        zpl_byte |= (1 << (7 - bit));
                    }
                }
            }
            sprintf(hex_ptr, "%02X", zpl_byte);
            hex_ptr += 2;
        }
    }

    /* Output ZPL */
    fprintf(stdout, "^XA\n");
    fprintf(stdout, "^FO0,0^GFA,%d,%d,%d,", total_bytes, total_bytes, zpl_bpr);
    fwrite(hex_buf, 1, (size_t)total_bytes * 2, stdout);
    fprintf(stdout, "\n^FS\n^XZ\n");
    fflush(stdout);

    /* Cleanup */
    free(hex_buf);
    CGContextRelease(ctx);
    free(buffer);
    CGPDFDocumentRelease(doc);

    if (tmp_path) {
        unlink(tmp_path);
        free(tmp_path);
    }

    return 0;
}
