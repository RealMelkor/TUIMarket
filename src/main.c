#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>
#include "termbox.h" 
#include "strlcpy.h" 

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define SIZEOF(X) sizeof(X)/sizeof(*X)

struct symbol {
	char symbol[16];
	char name[256];
	char state[128];
	float price;
	float previous_price;
	struct symbol *next;
};
struct symbol *symbols = NULL;

const char *query =
		"https://query1.finance.yahoo.com/v7/finance/"
		"quote?lang=en-US&region=US&corsDomain=finance.yahoo.com&"
		"fields=%s&symbols=%s";

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
		printf("not enough memory (realloc returned NULL)\n");
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
		fprintf(stderr, "curl_easy_perform() failed: %s\n",
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
		struct symbol s, *new;
		size_t len;

		if (!fgets(s.symbol, sizeof(s.symbol), f)) break;

		len = strnlen(s.symbol, sizeof(s.symbol));
		if (!len || len > sizeof(s.symbol)) break;

		if (s.symbol[len - 1] == '\n') s.symbol[len - 1] = '\0';

		new = malloc(sizeof(struct symbol));
		if (!new) return -1;

		*new = s;
		new->next = symbols;
		symbols = new;
	}

	return 0;
}

const char str_price[] = "\"regularMarketPrice\":";
const char str_old_price[] = "\"regularMarketPreviousClose\":";
const char str_state[] = "\"marketState\":\"";
const char str_name[] = "\"shortName\":\"";

static int find_copy(const char *haystack, const char *needle,
			size_t needle_len, char stop, char *buf, size_t len) {

	char *start, *end;

	start = strstr(haystack, needle);
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

	char buf[64], url[1024], *data;
	size_t len;
	int ret = -1;

	snprintf(url, sizeof(url), query,
			"regularMarketChange,regularMarketPrice,shortName,",
			symbol->symbol);

	data = handle_url(url, &len);
	if (!data) return -1;

	if (find_copy(data, str_price, sizeof(str_price), ',', buf,
				sizeof(buf)))
		goto clean;
	symbol->price = atof(buf);

	
	if (find_copy(data, str_old_price, sizeof(str_old_price), ',', buf,
				sizeof(buf)))
		goto clean;
	symbol->previous_price = atof(buf);

	if (find_copy(data, str_state, sizeof(str_state), '"', symbol->state,
				sizeof(symbol->state)))
		goto clean;

	if (find_copy(data, str_name, sizeof(str_name), '"', symbol->name,
				sizeof(symbol->name)))
		goto clean;

	ret = 0;
clean:
	free(data);

	if (symbol->next) update_symbol(symbol->next);

	return ret;
}

void *update_thread(void *ptr) {
	while (!ptr) {
		update_symbol(symbols);
		sleep(5);
	}
	return ptr;
}

const int col_symbol = 2;
const int col_name = col_symbol + sizeof("Symbol |");
const int col_variation = -(signed)sizeof("Variation") - 8;
const int col_price = col_variation -(signed)sizeof("| Price") - 3;

int main(int argc, char *argv[]) {

	int scroll = 0;
	pthread_t thread;

	if (!argc) return sizeof(*argv);

	if (load_symbols()) {
		printf("cannot find symbols file\n");
		return -1;
	}

	curl_global_init(CURL_GLOBAL_ALL);

	if (tb_init()) return -1;

	pthread_create(&thread, NULL, update_thread, NULL);

	while (1) {

		struct tb_event ev;
		struct symbol *symbol;
		int i, w, h, bottom;

		w = tb_width();
		h = tb_height();

		tb_clear();
		i = 0;
		while (i++ < w) tb_set_cell(i, 0, ' ', TB_BLACK, TB_WHITE);

		tb_print(col_symbol - 2, 0, TB_BLACK, TB_WHITE, " Symbol");
		tb_print(col_name - 2, 0, TB_BLACK, TB_WHITE, "| Name");
		tb_print(w + col_price - 2, 0, TB_BLACK, TB_WHITE, "| Price");
		tb_print(w + col_variation - 2, 0,
				TB_BLACK, TB_WHITE, "| Variation");
		
		i = 1;
		symbol = symbols;
		bottom = 1;
		while (symbol) {
			int gain;
			if (scroll >= i) {
				symbol = symbol->next;
				i++;
				continue;
			}
			gain = (symbol->price > symbol->previous_price);
			tb_print(col_symbol, i - scroll,
					TB_DEFAULT, TB_DEFAULT,
					symbol->symbol);
			tb_print(col_name, i - scroll, TB_DEFAULT, TB_DEFAULT,
					symbol->name);
			tb_printf(w + col_price, i - scroll,
					TB_DEFAULT, TB_DEFAULT,
					"%.2f", symbol->price);
			tb_printf(w + col_variation + gain, i - scroll,
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

		if (!tb_poll_event(&ev)) {
			if (ev.key == TB_KEY_ESC) break;
			if (ev.ch == 'q') break;
			if (ev.ch == 'j' && !bottom) scroll++;
			if (ev.ch == 'k' && scroll) scroll--;
		}

	}

	tb_shutdown();
	curl_global_cleanup();

	return 0;
}
