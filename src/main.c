#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <curl/curl.h>
#include "termbox.h" 
#include "strlcpy.h" 
#include "strnstr.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SIZEOF(X) sizeof(X) / sizeof(*X)

#define INTERVAL 5 /* update informations every x seconds */

struct symbol {
	char symbol[16];
	char name[256];
	float price;
	float previous_price;
	struct symbol *next;
};
struct symbol *symbols = NULL;

const char query_price[] =
	"https://query2.finance.yahoo.com/v7/finance/options/%s";

const char *paths[] = {
	".config/tuimarket/symbols",
	".tuimarket/symbols",
	".tuimarket_symbols",
};

struct mem {
	char *memory;
	size_t size;
};
struct mem chunk;

static size_t writecb(void *contents, size_t size, size_t nmemb, void *userp) {

	size_t realsize = size * nmemb;
	struct mem *mem = (struct mem*)userp;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if(mem->memory == NULL) {
		printf("not enough memory\n");
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}

char *handle_url(char *url, size_t *len) {

	CURL *curl_handle;
	CURLcode res;

	chunk.memory = malloc(1);
	chunk.size = 0;

	curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, writecb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&chunk);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	res = curl_easy_perform(curl_handle);
	curl_easy_cleanup(curl_handle);

	if(res != CURLE_OK) {
		printf("curl_easy_perform() failed: %s\n",
			curl_easy_strerror(res));
		return NULL;
	}

	*len = chunk.size;

	return chunk.memory;
}

static ssize_t get_home(char *buf, size_t length) {

        struct passwd *pw;
        char *home;
        int fd;

        home = getenv("HOME");
        if (home) {
                fd = open(home, O_DIRECTORY);
                if (fd > -1) {
                        close(fd);
                        return strlcpy(buf, home, length);
                }
        }

        pw = getpwuid(geteuid());
        if (!pw) return -1;
        fd = open(pw->pw_dir, O_DIRECTORY);
        if (fd < 0) {
                close(fd);
                return -1;
        }
        return strlcpy(buf, pw->pw_dir, length);
}

static int load_symbols() {

	FILE *f = NULL;
	size_t i;
	ssize_t len;
	char home[PATH_MAX], path[PATH_MAX];

	len = get_home(home, sizeof(home));
	if (len == -1) return -1;

	i = 0;
	while (i < SIZEOF(paths)) {
		snprintf(path, sizeof(path), "%s/%s", home, paths[i++]);
		f = fopen(path, "r");
		if (f) break;
	}
	if (!f) return -1;

	while (1) {

		struct symbol s = {0}, *new;
		size_t len;

		if (!fgets(s.symbol, sizeof(s.symbol), f)) break;

		len = strnlen(s.symbol, sizeof(s.symbol));
		if (!len || len > sizeof(s.symbol)) break;

		if (s.symbol[len - 1] == '\n') s.symbol[len - 1] = '\0';

		new = calloc(1, sizeof(struct symbol));
		if (!new) return -1;

		*new = s;
		new->next = symbols;
		symbols = new;
	}
	fclose(f);

	return 0;
}

const char str_price[] = "\"regularMarketPrice\":";
const char str_old_price[] = "\"regularMarketPreviousClose\":";
const char str_name[] = "\"shortName\":\"";

static int find_copy(const char *haystack, const char *needle, size_t hay_len,
			size_t needle_len, char stop, char *buf, size_t len) {

	char *start, *end;

	start = strnstr(haystack, needle, hay_len);
	if (!start) return -1;

	start += needle_len - 1;
	end = start;
	while (*end && *end != stop) end++;

	if (!*end || (size_t)(end - start) > len) return -1;

	memcpy(buf, start, end - start);
	buf[end - start] = '\0';

	return 0;
}

static int update_symbol(struct symbol *symbol) {

	char url[2048], buf[64], *data;
	size_t len;
	int ret = -1;

	snprintf(url, sizeof(url), query_price, symbol->symbol);
	data = handle_url(url, &len);
	if (!data) return -1;

	if (find_copy(data, str_price, len, sizeof(str_price), ',', buf,
			sizeof(buf)))
		goto clean;
	symbol->price = atof(buf);
	
	if (find_copy(data, str_old_price, len, sizeof(str_old_price), ',', buf,
			sizeof(buf)))
		goto clean;
	symbol->previous_price = atof(buf);

	if (find_copy(data, str_name, len, sizeof(str_name), '"',
			symbol->name, sizeof(symbol->name)))
		goto clean;

	ret = 0;
clean:
	free(data);

	return ret;
}

void ansi_sleep(long micro) {
        struct timeval tv;
        tv.tv_sec = micro / 1000000;
        tv.tv_usec = micro % 1000000;
        select(0, NULL, NULL, NULL, &tv);
}

void *update_thread(void *ptr) {
	int *run = ptr, interval, counter;
	size_t length;
	struct symbol *symbol;

	length = 0;
	for (symbol = symbols; symbol; symbol = symbol->next) length++;

	interval = 0;
	while (*run) {
		for (symbol = symbols; symbol; symbol = symbol->next) {
			update_symbol(symbol);
			counter = 0;
			while (counter++ < interval * 10 && *run)
				ansi_sleep(100000 / length);
			if (!*run) break;
		}
		interval = INTERVAL;
	}
	return ptr;
}

#define COL_SYMBOL 2
#define COL_NAME (COL_SYMBOL + sizeof("Symbol |") + 1)
#define COL_VARIATION (-(signed)sizeof("Variation") - 8)
#define COL_PRICE (COL_VARIATION -(signed)sizeof("| Price") - 3)

int main(int argc, char *argv[]) {

	int scroll = 0, run;
	pthread_t thread;

	if (!argc) return sizeof(*argv);

	if (load_symbols()) {
		printf("cannot find symbols file\n");
		return -1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	if (tb_init()) {
		printf("tb_init: %s\n", strerror(errno));
		return -1;
	}

	run = 1;
	pthread_create(&thread, NULL, update_thread, &run);

	while (1) {

		struct tb_event ev;
		struct symbol *symbol;
		int i, w, h, bottom;

		w = tb_width();
		h = tb_height();

		tb_clear();
		i = 0;
		while (i++ < w) tb_set_cell(i, 0, ' ', TB_BLACK, TB_WHITE);

		tb_print(COL_SYMBOL - 2, 0, TB_BLACK, TB_WHITE, " Symbol");
		tb_print(COL_NAME - 2, 0, TB_BLACK, TB_WHITE, "| Name");
		tb_print(w + COL_PRICE - 2, 0, TB_BLACK, TB_WHITE, "| Price");
		tb_print(w + COL_VARIATION - 2, 0,
				TB_BLACK, TB_WHITE, "| Variation");
		
		i = 1;
		symbol = symbols;
		bottom = 1;
		while (symbol) {
			int gain, j;
			if (scroll >= i) {
				symbol = symbol->next;
				i++;
				continue;
			}
			gain = (symbol->price >= symbol->previous_price);
			tb_print(COL_SYMBOL, i - scroll,
					TB_DEFAULT, TB_DEFAULT,
					symbol->symbol);
			tb_print(COL_NAME, i - scroll, TB_DEFAULT, TB_DEFAULT,
					symbol->name);

			j = w + COL_PRICE - 2;
			while (j++ < w)
				tb_set_cell(j, i, ' ', TB_DEFAULT, TB_DEFAULT);
			tb_printf(w + COL_PRICE, i - scroll,
					TB_DEFAULT, TB_DEFAULT,
					"%.2f", symbol->price);
			tb_printf(w + COL_VARIATION + gain, i - scroll,
					gain ? TB_GREEN : TB_RED,
					TB_DEFAULT, "%.2f (%.2f%%)",
					symbol->price - symbol->previous_price,
					symbol->price / symbol->previous_price
					* 100 - 100);
			symbol = symbol->next;
			if (i - scroll > h) {
				bottom = 0;
				break;
			}
			i++;
		}

		tb_present();

		if (!tb_peek_event(&ev, 1000)) {
			if (ev.key == TB_KEY_ESC || ev.ch == 'q') break;
			if ((ev.key == TB_KEY_ARROW_DOWN || ev.ch == 'j') &&
				!bottom) scroll++;
			if ((ev.key == TB_KEY_ARROW_UP || ev.ch == 'k') &&
				scroll) scroll--;
		}

	}

	run = 0;
	tb_shutdown();
	pthread_join(thread, NULL);
	curl_global_cleanup();
	while (symbols) {
		struct symbol *s = symbols;
		symbols = symbols->next;
		free(s);
	}

	return 0;
}
