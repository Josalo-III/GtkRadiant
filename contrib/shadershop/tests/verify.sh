#!/bin/sh
# Build and run ShaderShop's ScriptLib stage-walk verification against a shader corpus.
#
#   ./verify.sh <directory containing .shader files>
#   ./verify.sh --self-test
#
# The harness calls Radiant's parser through the same ScriptLib table contract
# as the plugin.  Its fixture is a small semantic regression test; its corpus
# mode is a census, not a claim that every directive has full preview support.

set -e
here=$( cd "$( dirname "$0" )" && pwd )
root=$( cd "$here/../../.." && pwd )
mode=${1:-}
corpus=$root/install/installs
if [ "$mode" != "--self-test" ] && [ -n "$mode" ]; then
	corpus=$mode
fi
out=$here/build
mkdir -p "$out"

# Radiant's tokenizer: declarations through the end of GetToken, plus its
# single-token pushback helper.
awk 'NR>=24 && NR<=131' "$root/radiant/parse.cpp" | grep -v '#include' > "$out/radiant_parse.inc"
sed -n '/^void UngetToken/,/^}/p'      "$root/radiant/parse.cpp" >> "$out/radiant_parse.inc"

c++ -std=c++98 -O1 -I"$out" -I"$root/include" -o "$out/shadershop_verify" "$here/shadershop_verify.cpp"

if [ "$mode" = "--self-test" ]; then
	"$out/shadershop_verify" --stages "$here/fixtures/stagewalk.shader" | diff -u "$here/fixtures/stagewalk.expected" -
	echo "ShaderShop ScriptLib stage-walk fixture: passed"
	exit 0
fi

echo "== ShaderShop ScriptLib stage-walk corpus census =="
find "$corpus" -name '*.shader' | while read f; do "$out/shadershop_verify" --census "$f"; done \
| awk -F'\t' '{for(i=1;i<=11;i++) s[i]+=$i} END{
	printf "shader definitions         %8d\n", s[1]
	printf "stages                     %8d\n", s[2]
	printf "blendFunc explicit         %8d\n", s[3]
	printf "blendFunc shorthand        %8d\n", s[4]
	printf "blendFunc unknown form     %8d\n", s[5]
	printf "blend factor diagnostics   %8d\n", s[6]
	printf "map $lightmap              %8d\n", s[7]
	printf "map $whiteimage            %8d\n", s[8]
	printf "clampmap                   %8d\n", s[9]
	printf "animMap                    %8d\n", s[10]
	printf "tcMod directives           %8d\n", s[11] }'
