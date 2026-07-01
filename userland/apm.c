#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

void usage(void) {
    printf("Ascent Package Manager (apm) - AUR Client\n");
    printf("Usage: apm <command> [argument]\n\n");
    printf("Commands:\n");
    printf("  search <keyword>   Search for packages on AUR\n");
    printf("  info <package>     Show package details\n");
    printf("  install <package>  Download, compile, and install package\n");
    printf("  list               List installed packages\n");
    printf("  remove <package>   Uninstall package (remove marker)\n");
    printf("  help               Show this help message\n");
}

char *json_get_value(const char *json, const char *key, char *buf, int max_len) {
    char key_pattern[128];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\":", key);
    const char *p = strstr(json, key_pattern);
    if (!p) return NULL;
    p += strlen(key_pattern);
    
    // Skip whitespace
    while (*p == ' ' || *p == '\t') p++;
    
    if (*p == '"') {
        p++; // skip leading double quote
        int index = 0;
        while (*p && *p != '"' && index < max_len - 1) {
            if (*p == '\\' && *(p+1) == '/') {
                buf[index++] = '/';
                p += 2;
            } else if (*p == '\\' && *(p+1) == '"') {
                buf[index++] = '"';
                p += 2;
            } else {
                buf[index++] = *p++;
            }
        }
        buf[index] = '\0';
    } else {
        // Value is a number or null/true/false
        int index = 0;
        while (*p && *p != ',' && *p != '}' && *p != ']' && index < max_len - 1) {
            buf[index++] = *p++;
        }
        buf[index] = '\0';
    }
    return buf;
}

char *next_object(char **cursor) {
    char *p = *cursor;
    while (*p && *p != '{') {
        if (*p == ']') return NULL;
        p++;
    }
    if (!*p) return NULL;
    
    char *start = p;
    int brace_count = 0;
    while (*p) {
        if (*p == '{') brace_count++;
        else if (*p == '}') {
            brace_count--;
            if (brace_count == 0) {
                char *end = p + 1;
                *cursor = end;
                int len = end - start;
                char *obj = malloc(len + 1);
                memcpy(obj, start, len);
                obj[len] = '\0';
                return obj;
            }
        }
        p++;
    }
    return NULL;
}

void search_package(const char *keyword) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -sL \"https://aur.archlinux.org/rpc/?v=5&type=search&arg=%s\" -o /tmp/apm_search.json", keyword);
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Error: curl query failed.\n");
        return;
    }
    
    FILE *f = fopen("/tmp/apm_search.json", "r");
    if (!f) {
        fprintf(stderr, "Error: failed to open search result.\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(size + 1);
    int read_bytes = fread(json, 1, size, f);
    json[read_bytes] = '\0';
    fclose(f);
    
    char *array_ptr = strstr(json, "\"results\":[");
    if (!array_ptr) {
        printf("No results found.\n");
        free(json);
        return;
    }
    array_ptr += 11;
    
    printf("%-24s %-12s %s\n", "NAME", "VERSION", "DESCRIPTION");
    printf("--------------------------------------------------------------------------------\n");
    char *obj;
    int count = 0;
    while ((obj = next_object(&array_ptr)) != NULL) {
        char name[128] = {0};
        char version[64] = {0};
        char desc[512] = {0};
        
        json_get_value(obj, "Name", name, sizeof(name));
        json_get_value(obj, "Version", version, sizeof(version));
        json_get_value(obj, "Description", desc, sizeof(desc));
        
        if (strlen(name) > 0) {
            printf("%-24s %-12s %s\n", name, version, desc);
            count++;
        }
        free(obj);
    }
    if (count == 0) {
        printf("No packages found matching query.\n");
    }
    free(json);
}

void info_package(const char *pkgname) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -sL \"https://aur.archlinux.org/rpc/?v=5&type=info&arg=%s\" -o /tmp/apm_info.json", pkgname);
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Error: curl query failed.\n");
        return;
    }
    
    FILE *f = fopen("/tmp/apm_info.json", "r");
    if (!f) {
        fprintf(stderr, "Error: failed to open info result.\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(size + 1);
    int read_bytes = fread(json, 1, size, f);
    json[read_bytes] = '\0';
    fclose(f);
    
    char *array_ptr = strstr(json, "\"results\":[");
    if (!array_ptr) {
        printf("Package not found.\n");
        free(json);
        return;
    }
    array_ptr += 11;
    
    char *obj = next_object(&array_ptr);
    if (!obj) {
        printf("Package '%s' not found.\n", pkgname);
        free(json);
        return;
    }
    
    char name[128] = {0};
    char version[64] = {0};
    char desc[512] = {0};
    char url[256] = {0};
    char maintainer[128] = {0};
    char popularity[32] = {0};
    
    json_get_value(obj, "Name", name, sizeof(name));
    json_get_value(obj, "Version", version, sizeof(version));
    json_get_value(obj, "Description", desc, sizeof(desc));
    json_get_value(obj, "URL", url, sizeof(url));
    json_get_value(obj, "Maintainer", maintainer, sizeof(maintainer));
    json_get_value(obj, "Popularity", popularity, sizeof(popularity));
    
    printf("Package     : %s\n", name);
    printf("Version     : %s\n", version);
    printf("Description : %s\n", desc);
    printf("URL         : %s\n", url);
    printf("Maintainer  : %s\n", maintainer);
    printf("Popularity  : %s\n", popularity);
    
    free(obj);
    free(json);
}

void install_package(const char *pkgname) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -sL \"https://aur.archlinux.org/rpc/?v=5&type=info&arg=%s\" -o /tmp/apm_info.json", pkgname);
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Error: failed to fetch package info.\n");
        return;
    }
    
    FILE *f = fopen("/tmp/apm_info.json", "r");
    if (!f) {
        fprintf(stderr, "Error: failed to open package info.\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *json = malloc(size + 1);
    int read_bytes = fread(json, 1, size, f);
    json[read_bytes] = '\0';
    fclose(f);
    
    char *array_ptr = strstr(json, "\"results\":[");
    if (!array_ptr) {
        fprintf(stderr, "Error: invalid response from AUR.\n");
        free(json);
        return;
    }
    array_ptr += 11;
    char *obj = next_object(&array_ptr);
    if (!obj) {
        fprintf(stderr, "Error: package '%s' not found on AUR.\n", pkgname);
        free(json);
        return;
    }
    
    char urlpath[256] = {0};
    json_get_value(obj, "URLPath", urlpath, sizeof(urlpath));
    
    if (strlen(urlpath) == 0) {
        fprintf(stderr, "Error: package snapshot URL invalid.\n");
        free(obj);
        free(json);
        return;
    }
    
    free(obj);
    free(json);
    
    char download_url[512];
    snprintf(download_url, sizeof(download_url), "https://aur.archlinux.org%s", urlpath);
    
    printf("Downloading package snapshot from AUR...\n");
    snprintf(cmd, sizeof(cmd), "curl -sL \"%s\" -o /tmp/%s.tar.gz", download_url, pkgname);
    status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Error: download failed.\n");
        return;
    }
    
    printf("Extracting snapshot...\n");
    snprintf(cmd, sizeof(cmd), "mkdir -p /tmp && tar -xzf /tmp/%s.tar.gz -C /tmp/", pkgname);
    status = system(cmd);
    if (status != 0) {
        fprintf(stderr, "Error: extraction failed.\n");
        return;
    }
    
    char script_path[256];
    snprintf(script_path, sizeof(script_path), "/tmp/apm_build_%s.sh", pkgname);
    FILE *sf = fopen(script_path, "w");
    if (!sf) {
        fprintf(stderr, "Error: failed to create build script.\n");
        return;
    }
    
    /* POSIX/ash-compatible build script - no bash-isms */
    fprintf(sf, "#!/bin/sh\n");
    fprintf(sf, "set -e\n");
    fprintf(sf, "PKGBUILD_DIR=/tmp/%s\n", pkgname);
    fprintf(sf, "cd \"$PKGBUILD_DIR\"\n");
    fprintf(sf, "[ -f PKGBUILD ] || { echo 'No PKGBUILD found!'; exit 1; }\n");
    /* Use '.' (dot) which is POSIX sh equivalent of bash 'source' */
    fprintf(sf, ". ./PKGBUILD\n");
    fprintf(sf, "echo \"Building AUR Package: $pkgname version $pkgver\"\n");
    /* Parse sources from .SRCINFO (plain key=value, no bash arrays needed) */
    fprintf(sf, "if [ -f .SRCINFO ]; then\n");
    fprintf(sf, "  grep '^ *source *=' .SRCINFO | while IFS= read -r _srcline; do\n");
    fprintf(sf, "    _val=$(echo \"$_srcline\" | sed 's/^ *source *= *//')\n");
    fprintf(sf, "    _url=$(echo \"$_val\" | sed 's/^[^:]*:://')\n");
    fprintf(sf, "    case \"$_url\" in\n");
    fprintf(sf, "      http://*|https://*)\n");
    fprintf(sf, "        _fname=$(echo \"$_val\" | sed 's/::.*//')\n");
    fprintf(sf, "        case \"$_fname\" in http://*|https://*) _fname=$(basename \"$_url\") ;; esac\n");
    fprintf(sf, "        echo \"Downloading: $_url\"\n");
    fprintf(sf, "        curl -L \"$_url\" -o \"$_fname\"\n");
    fprintf(sf, "        ;;\n");
    fprintf(sf, "    esac\n");
    fprintf(sf, "  done\n");
    fprintf(sf, "fi\n");
    /* Extract any archives */
    fprintf(sf, "for f in *.tar.gz *.tgz; do [ -f \"$f\" ] && tar -xzf \"$f\"; done\n");
    fprintf(sf, "for f in *.tar.xz *.txz; do [ -f \"$f\" ] && tar -xJf \"$f\"; done\n");
    fprintf(sf, "for f in *.zip; do [ -f \"$f\" ] && unzip \"$f\"; done\n");
    /* srcdir points to the build dir; PKGBUILD build() functions cd into $srcdir/pkgname-pkgver */
    fprintf(sf, "srcdir=\"$PKGBUILD_DIR\"\n");
    fprintf(sf, "export srcdir\n");
    /* Call prepare/build/package if they are defined as shell functions */
    fprintf(sf, "if type prepare 2>/dev/null | grep -q 'function\\|()'; then\n");
    fprintf(sf, "  echo \"Running prepare()...\"; prepare\n");
    fprintf(sf, "fi\n");
    fprintf(sf, "if type build 2>/dev/null | grep -q 'function\\|()'; then\n");
    fprintf(sf, "  echo \"Running build()...\"; build\n");
    fprintf(sf, "fi\n");
    fprintf(sf, "pkgdir=\"/tmp/%s/pkg\"\n", pkgname);
    fprintf(sf, "export pkgdir\n");
    fprintf(sf, "mkdir -p \"$pkgdir\"\n");
    fprintf(sf, "if type package 2>/dev/null | grep -q 'function\\|()'; then\n");
    fprintf(sf, "  echo \"Running package()...\"; package\n");
    fprintf(sf, "fi\n");
    fprintf(sf, "if [ -d \"$pkgdir\" ] && ls \"$pkgdir\" | grep -q .; then\n");
    fprintf(sf, "  echo \"Installing compiled files to system root...\"; cp -rv \"$pkgdir\"/* /\n");
    fprintf(sf, "else\n");
    fprintf(sf, "  echo \"Warning: pkgdir empty, nothing to install.\"; fi\n");
    fclose(sf);

    /* Execute via /bin/sh directly - do not rely on execute bit or PATH lookup */
    printf("Starting build/install process for %s...\n", pkgname);
    snprintf(cmd, sizeof(cmd), "/bin/sh %s", script_path);
    status = system(cmd);
    if (status == 0) {
        printf("\nPackage '%s' successfully compiled and installed!\n", pkgname);
        snprintf(cmd, sizeof(cmd), "mkdir -p /etc/apm/installed && touch /etc/apm/installed/%s", pkgname);
        system(cmd);
    } else {
        fprintf(stderr, "Error: package build/installation failed.\n");
    }
}

void list_packages(void) {
    printf("Installed packages:\n");
    system("ls -1 /etc/apm/installed/ 2>/dev/null || echo '(none)'");
}

void remove_package(const char *pkgname) {
    char path[256];
    snprintf(path, sizeof(path), "/etc/apm/installed/%s", pkgname);
    if (unlink(path) == 0) {
        printf("Uninstalled %s (removed tracking marker). Note: built files remain on disk.\n", pkgname);
    } else {
        printf("Package '%s' is not installed or could not be removed.\n", pkgname);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    
    const char *cmd = argv[1];
    if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: please specify search query.\n");
            return 1;
        }
        search_package(argv[2]);
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: please specify package name.\n");
            return 1;
        }
        info_package(argv[2]);
    } else if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: please specify package name.\n");
            return 1;
        }
        install_package(argv[2]);
    } else if (strcmp(cmd, "list") == 0) {
        list_packages();
    } else if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: please specify package name.\n");
            return 1;
        }
        remove_package(argv[2]);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
    } else {
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        usage();
        return 1;
    }
    
    return 0;
}
