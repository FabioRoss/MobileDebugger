#pragma once
#include <SPI.h>
#include <SD.h>
#include <FS.h>

/* Dedicated SPI bus for the TF slot so it never fights the QSPI display.
 * (On the resistive-touch variant this bus is shared with the XPT2046, so
 * SD and resistive touch can't both be live — the capacitive variant is
 * unaffected since its touch is I2C.)                                       */

SPIClass sdSPI;
bool     sd_ok = false;

struct FileEntry {
    char     name[80];
    uint32_t size;
    bool     isDir;
};

#define SD_MAX_ENTRIES 96
FileEntry sd_entries[SD_MAX_ENTRIES];
int       sd_entry_count = 0;
char      sd_cwd[160] = "/";

static bool ends_with_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return false;
    for (size_t i = 0; i < lf; i++)
        if (tolower((unsigned char)s[ls - lf + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

bool sd_mount() {
    if (sd_ok) return true;
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, SD_CLOCK_SPEED)) {
        Serial.println("[SD] init failed");
        sdSPI.end();
        sd_ok = false;
        return false;
    }
    Serial.println("[SD] mounted");
    sd_ok = true;
    return true;
}

const char *sd_type_str() {
    switch (SD.cardType()) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC";
        default:        return "none";
    }
}

/* join sd_cwd + child into dst, collapsing the root double-slash */
static void sd_join(char *dst, size_t n, const char *dir, const char *child) {
    if (!strcmp(dir, "/")) snprintf(dst, n, "/%s", child);
    else                   snprintf(dst, n, "%s/%s", dir, child);
}

/* list current dir, dirs first, showing only sub-dirs + .bin + .json files */
bool sd_list() {
    sd_entry_count = 0;
    if (!sd_ok && !sd_mount()) return false;

    File dir = SD.open(sd_cwd);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

    if (strcmp(sd_cwd, "/") != 0 && sd_entry_count < SD_MAX_ENTRIES) {
        FileEntry &e = sd_entries[sd_entry_count++];
        strcpy(e.name, ".."); e.isDir = true; e.size = 0;
    }

    /* first pass: directories */
    File f;
    while ((f = dir.openNextFile())) {
        if (sd_entry_count >= SD_MAX_ENTRIES) break;
        const char *nm = f.name();
        const char *slash = strrchr(nm, '/');
        if (slash) nm = slash + 1;
        if (f.isDirectory() && nm[0] != '.') {
            FileEntry &e = sd_entries[sd_entry_count++];
            strncpy(e.name, nm, sizeof(e.name) - 1); e.name[sizeof(e.name)-1] = 0;
            e.isDir = true; e.size = 0;
        }
        f.close();
    }
    dir.close();

    /* second pass: flashable files */
    dir = SD.open(sd_cwd);
    while ((f = dir.openNextFile())) {
        if (sd_entry_count >= SD_MAX_ENTRIES) break;
        const char *nm = f.name();
        const char *slash = strrchr(nm, '/');
        if (slash) nm = slash + 1;
        if (!f.isDirectory() && (ends_with_ci(nm, ".bin") || ends_with_ci(nm, ".json"))) {
            FileEntry &e = sd_entries[sd_entry_count++];
            strncpy(e.name, nm, sizeof(e.name) - 1); e.name[sizeof(e.name)-1] = 0;
            e.isDir = false; e.size = (uint32_t)f.size();
        }
        f.close();
    }
    dir.close();
    return true;
}

void sd_cd(const char *name) {
    if (!strcmp(name, "..")) {
        char *slash = strrchr(sd_cwd, '/');
        if (slash) { if (slash == sd_cwd) sd_cwd[1] = 0; else *slash = 0; }
    } else {
        char tmp[160];
        sd_join(tmp, sizeof(tmp), sd_cwd, name);
        strncpy(sd_cwd, tmp, sizeof(sd_cwd) - 1); sd_cwd[sizeof(sd_cwd)-1] = 0;
    }
}
