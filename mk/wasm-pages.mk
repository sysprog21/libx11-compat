# GitHub Pages bundle for WebAssembly showcases.

WASM_PAGES_EXAMPLES := $(WASM_EXAMPLES)
WASM_PAGES_SHOWCASES := xnedit xwpe mosaic xcircuit
WASM_PAGES_APPS := $(WASM_PAGES_EXAMPLES) $(WASM_PAGES_SHOWCASES)

ifeq ($(WASM),1)

WASM_PAGES_DIR := $(OUT)/pages
WASM_PAGES_HTML := $(addprefix $(OUT)/,$(addsuffix .html,$(WASM_PAGES_APPS)))
WASM_PAGES_STAMP := $(WASM_PAGES_DIR)/.stamp

$(WASM_PAGES_STAMP): $(WASM_PAGES_HTML) mk/wasm-pages.mk
	@echo "  PAGES   $(WASM_PAGES_DIR)"
	$(Q)rm -rf $(WASM_PAGES_DIR)
	$(Q)mkdir -p $(WASM_PAGES_DIR)
	$(Q)touch $(WASM_PAGES_DIR)/.nojekyll
	$(Q)for app in $(WASM_PAGES_APPS); do \
	    for ext in html js wasm data; do \
	        f="$(OUT)/$$app.$$ext"; [ ! -s "$$f" ] || cp "$$f" "$(WASM_PAGES_DIR)/"; \
	    done; \
	done
	$(Q){ \
	    printf '%s\n' '<!doctype html>' '<meta charset="utf-8">' \
	        '<meta name="viewport" content="width=device-width,initial-scale=1">' \
	        '<title>libx11-compat WebAssembly showcases</title>' \
	        '<style>body{font:16px system-ui,sans-serif;max-width:760px;margin:40px auto;padding:0 20px;line-height:1.5}h1{font-size:28px}h2{font-size:20px;margin-top:28px}a{color:#0645ad}a.ref{color:#666;font-size:13px;text-decoration:none;margin-left:8px}</style>' \
	        '<h1>libx11-compat WebAssembly showcases</h1>' \
	        '<p>Unmodified X11 clients cross-compiled to WebAssembly, running with no X server. Source: <a href="https://github.com/sysprog21/libx11-compat">github.com/sysprog21/libx11-compat</a></p>' \
	        '<h2>Examples</h2>' '<ul>'; \
	    for item in \
	        'paint:Paint' 'life:Life' 'clock:Clock' 'mandel:Mandelbrot' \
	        'processing:Processing' 'clipboard:Clipboard' 'catclock:Catclock' \
	        'moire:Moire'; do \
	        app="$${item%%:*}"; title="$${item#*:}"; \
	        printf '<li><a href="%s.html">%s</a><a class="ref" href="https://github.com/sysprog21/libx11-compat/blob/main/examples/%s.c">source</a></li>\n' "$$app" "$$title" "$$app"; \
	    done; \
	    printf '%s\n' '</ul>' '<h2>Showcases</h2>' '<ul>'; \
	    for item in \
	        'xnedit|XNEdit|https://github.com/unixwork/xnedit' \
	        'xwpe|xwpe|https://github.com/vejeta/xwpe' \
	        'mosaic|Mosaic|https://github.com/thentenaar/mosaic-ng' \
	        'xcircuit|XCircuit|http://opencircuitdesign.com/xcircuit/'; do \
	        app="$${item%%|*}"; rest="$${item#*|}"; title="$${rest%%|*}"; url="$${rest#*|}"; \
	        printf '<li><a href="%s.html">%s</a><a class="ref" href="%s">project</a></li>\n' "$$app" "$$title" "$$url"; \
	    done; \
	    printf '%s\n' '</ul>'; \
	} > $(WASM_PAGES_DIR)/index.html
	$(Q)touch $@

.PHONY: wasm-pages
wasm-pages: $(WASM_PAGES_STAMP)

endif

.PHONY: check-wasm-pages
## Build the GitHub Pages bundle for wasm examples/showcases
check-wasm-pages:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-pages (emcc not found)"; exit 0; fi; \
	$(MAKE) WASM=1 wasm-pages || exit 1; \
	if [ ! -s "$(OUT)/pages/index.html" ]; then \
	    echo "check-wasm-pages: missing index.html" >&2; exit 1; \
	fi; \
	for app in $(WASM_PAGES_APPS); do \
	    for ext in html js wasm; do \
	        if [ ! -s "$(OUT)/pages/$$app.$$ext" ]; then \
	            echo "check-wasm-pages: missing or empty $$app.$$ext" >&2; \
	            exit 1; \
	        fi; \
	    done; \
	    case `file -b "$(OUT)/pages/$$app.wasm"` in \
	        *WebAssembly*) ;; \
	        *) echo "check-wasm-pages: $$app.wasm is not a wasm module" >&2; \
	           exit 1;; \
	    esac; \
	done; \
	for app in $(WASM_PAGES_SHOWCASES); do \
	    if [ ! -s "$(OUT)/pages/$$app.data" ]; then \
	        echo "check-wasm-pages: missing or empty $$app.data" >&2; exit 1; \
	    fi; \
	done; \
	echo "  OK      check-wasm-pages ($(WASM_PAGES_APPS))"
