# SereneDB fork

This fork of [asubbarao/duckdb-pdf](https://github.com/asubbarao/duckdb-pdf/tree/c47d46cc99c73d87d6f15561282820b581929cc1) provides a PoDoFo backend for SereneDB's DuckDB fork.

SereneDB selects `PDF_USE_PODOFO=ON`. This backend provides `read_pdf` and `read_pdf_blocks`, with file globs, passwords, and inclusive page ranges. Page text defaults to geometric reading order; `layout := 'raw'` preserves content order. OCR, table extraction, and document editing are unavailable in this mode. The upstream README describes the Poppler backend, which remains the default outside SereneDB.

Build and test through the parent SereneDB checkout. Its generated PDF fixtures and SQL suites cover page and block extraction, malformed files, encryption, Unicode, blank and scanned pages, rotated pages, and multi-chunk output. Its shell tests cover extension loading without a server.

The parent supplies DuckDB and PoDoFo; nested DuckDB and extension-ci-tools submodules are omitted. The upstream license and bundled notices remain unchanged. SereneDB's added PoDoFo reader retains its Apache-2.0 notice.
