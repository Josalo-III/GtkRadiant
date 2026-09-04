#!/bin/sh
# Build and run ShaderShop's ScriptLib stage-walk verification against a shader corpus.
#
#   ./verify.sh <directory containing .shader files>
#
# Compiles Radiant's parser into the harness and reports a corpus census from
# the ScriptLib token stream used by ShaderShop in production.

set -e
here=$( cd "$( dirname "$0" )" && pwd )
root=$( cd "$here/../../.." && pwd )
corpus=${1:-$root/install/installs}
out=$here/build
mkdir -p "$out"

# Radiant's tokenizer: declarations through the end of GetToken, plus its
# single-token pushback helper.
awk 'NR>=24 && NR<=131' "$root/radiant/parse.cpp" | grep -v '#include' > "$out/radiant_parse.inc"
sed -n '/^void UngetToken/,/^}/p'      "$root/radiant/parse.cpp" >> "$out/radiant_parse.inc"

c++ -std=c++98 -O1 -I"$out" -o "$out/shadershop_verify" "$here/shadershop_verify.cpp"

echo "== ScriptLib stage-walk corpus census =="
find "$corpus" -name '*.shader' | while read f; do "$out/shadershop_verify" --census "$f"; done \
| awk -F'\t' '{for(i=1;i<=11;i++) s[i]+=$i} END{
	printf "shader definitions         %8d\n", s[1]
	printf "blendFunc in stages        %8d\n", s[2]
	printf "  keyword case unmatched   %8d\n", s[3]
	printf "  shorthand dropped        %8d\n", s[4]
	printf "  factor unrecognised      %8d\n", s[5]
	printf "    of which SRC_COLOR     %8d\n", s[7]
	printf "  wrong blend state        %8d  (%.1f%%)\n", s[6], s[2] ? 100*s[6]/s[2] : 0
	printf "map $lightmap              %8d\n", s[8]
	printf "map $whiteimage            %8d\n", s[9]
	printf "clampmap                   %8d\n", s[10]
	printf "animMap                    %8d\n", s[11] }'
