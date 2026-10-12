#!/bin/bash
# test.sh — automated check of the folder scanner pipeline.
# Usage: make test      (or ./test.sh after make all)

SOCK=/tmp/crawl_test.sock
PASS=0; FAIL=0

# scan <crawler options...> : starts the indexer, runs the crawler, waits for the indexer.
# The crawler's summary is saved in $SUMMARY.
scan() {
    rm -rf data/pages/* data/index/* "$SOCK"
    ./indexer --ipc "$SOCK" --out data/index >/dev/null 2>&1 &
    local idx=$!
    sleep 0.5
    SUMMARY=$(./crawler --out data --ipc "$SOCK" "$@" 2>/dev/null)
    wait $idx
}

# files <term...> : prints the (sorted) file paths matching all terms
files() { ./query --index data/index "$@" 2>/dev/null | grep -v "^Found\|^No documents" | cut -d' ' -f2 | sort; }
count() { echo "$SUMMARY" | grep "$1" | awk -F: '{gsub(/ /,"",$2); print $2}'; }

check() {   # check <name> <expected> <actual>
    if [ "$2" == "$3" ]; then echo "PASS: $1"; PASS=$((PASS+1));
    else echo "FAIL: $1"; echo "   expected: $2"; echo "   actual:   $3"; FAIL=$((FAIL+1)); fi
}

[ -x crawler ] && [ -x indexer ] && [ -x query ] || { echo "Run 'make all' first"; exit 1; }

# 1. default scan
scan --root testdata --max-depth 3 --max-pages 100 -t 4
check "default: files indexed"  "8" "$(count 'Files indexed')"
check "default: files skipped"  "1" "$(count 'Files skipped')"
check "default: files failed"   "0" "$(count 'Files failed')"
check "AND query (operating systems threads)" \
      "testdata/notes/week1/synchronization.txt
testdata/os/threads.txt" "$(files operating systems threads)"
check "query deadlock" \
      "testdata/notes/week1/extra/deadlock.txt
testdata/notes/week1/synchronization.txt" "$(files deadlock)"
check "word in .md file is not indexed" "" "$(files xylophone)"
check "hidden file is not indexed"      "" "$(files quokka)"
check "unknown word gives no results"   "" "$(files thistermdoesnotexist)"
check "saved copies exist" "8" "$(ls data/pages | wc -l | tr -d ' ')"

# 2. 1 thread and 8 threads must find exactly the same files
scan --root testdata --max-depth 3 --max-pages 100 -t 1
ONE=$(files threads)
scan --root testdata --max-depth 3 --max-pages 100 -t 8
check "1 thread == 8 threads" "$ONE" "$(files threads)"

# 3. depth limit: --max-depth 1 reaches networks/ and os/ but not week1/ or memory/
scan --root testdata --max-depth 1 --max-pages 100 -t 4
check "max-depth 1: files indexed" "4" "$(count 'Files indexed')"
check "max-depth 1: deeper file not indexed" "" "$(files deadlock)"
scan --root testdata --max-depth 0 --max-pages 100 -t 4
check "max-depth 0: files indexed" "0" "$(count 'Files indexed')"

# 4. file limit
scan --root testdata --max-depth 3 --max-pages 3 -t 4
check "max-pages 3: files indexed" "3" "$(count 'Files indexed')"

# 5. extension filter
scan --root testdata --max-depth 3 --max-pages 100 -t 4 --ext md
check "--ext md: only the .md file" "testdata/notes/readme.md" "$(files xylophone)"
check "--ext md: files indexed" "1" "$(count 'Files indexed')"

# 6. size filter (every sample file is bigger than 10 bytes)
scan --root testdata --max-depth 3 --max-pages 100 -t 4 --max-size 10
check "--max-size 10: files indexed" "0" "$(count 'Files indexed')"
check "--max-size 10: files skipped" "9" "$(count 'Files skipped')"

# 7. bad root folder
./crawler --root no_such_folder --out data --ipc "$SOCK" >/dev/null 2>&1
check "missing root folder gives exit code 1" "1" "$?"

rm -f "$SOCK"
echo; echo "Passed: $PASS   Failed: $FAIL"
[ "$FAIL" -eq 0 ]
