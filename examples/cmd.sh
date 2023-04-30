#!/bin/bash
#
#  oooo  oooo
#  `888  `888
#   888   888  ooo. .oo.  .oo.    .ooooo oo
#   888   888  `888P"Y88bP"Y88b  d88' `888
#   888   888   888   888   888  888   888
#  o888o o888o o888o o888o o888o `V8bod888
#                                      888.
#  a query CLI, plugin framework, and  8P'
#  I/O manager for conversational AIs  "
#
#  Copyright (C) 2023 Justin Collier <m@jpcx.dev>
#
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Affero General Public License as
#    published by the Free Software Foundation, either version 3 of the
#    License, or (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <https://www.gnu.org/licenses/>.

# !! THIS IS AN EXPERIMENTAL UTILITY !!
#
# I am not responsible for any changes you make to your system by using this tool.
# It is not foolproof, and often makes mistakes. But if you are very specific with
# your requests it can sometimes do well.
#
# Always run this in a sandbox!!!!

set -m

cleanup() {
	rm -f ~cmd.fifo
	pgrep -P $$ | xargs -r kill >/dev/null 2>&1
	jobs -p | xargs -r kill >/dev/null 2>&1
	exit 1
}

trap cleanup INT
trap cleanup TERM

interpret="GPT-4, you are the first stage in a bash command-line generation
pipeline. Your task is to interpret the user's request and respond with the
following structure: [OK] <descr...> if the request is specific and safe,
or [ERR] <why...> if the command is ambiguous or dangerous. You should not
actually generate the bash command- your task is to ensure that the user is
being specific enough. If the input is acceptable, add as many details to
the [OK] description as possible in order to disambiguate the request for
the generator. If not specified, assume the operation should be performed in
the cwd and should be shallow (e.g. for commands such as find). Be specific
about whether the targets should be files or directories (or either)."

if [ $# -eq 0 ]
then
	input="$(cat)"
else
	input="$@"
fi

rm -f ~cmd.fifo
mkfifo ~cmd.fifo

echo "$input" | llmq q gpt -m gpt-4 -S true -T 1.0 -s "$interpret" | tee ~cmd.fifo &
request="$(cat < ~cmd.fifo)"

if [[ $request == "[OK] "* ]]
then
	request="${request:5}"
elif [[ $request == "[ERR] "* ]]
then
	exit 1
else
	echo "error: interpret stage did not use the correct prefix." >&2
	exit 1
fi

generate="GPT-4, you are the second stage in a bash command-line generation
pipeline. Your task is to interpret the user's request and generate the exact
bash command required. Your output will be directly evaluated by bash and must
be syntactically correct, without exception. Keep in mind that it will be sent
to bash via stdin. The user's original request was \"$input\" before being
sanitized to the input you will receive."

printf "\ngenerating...\n\n"

echo "$request" | llmq q gpt -m gpt-4 -S true -T 0.0 -s "$generate" | tee ~cmd.fifo &
gen="$(cat < ~cmd.fifo)"

printf "\nvalidating...\n\n"

validate="GPT-4, you are the final stage in a bash command-line generation
pipeline. The user has asked another instance of you to generate a command,
and your job is to double-check that the command is safe and meets the
expectations of the user's request. If you have an issue with the command,
print \"[ERR] <why....>\" if the user request should be revised, \"[EDIT]
<new cmd...>\" if there are any revisions that should be made, or \"[OK]\"
if you are absolutely sure that the command is optimal. Prever to make changes
to the input command, unless you are absolutely sure that the command is optimal.
The user's original request was \"$input\" before being sanitized to \"$request\"
prior to generation."

echo "$gen" | llmq q gpt -m gpt-4 -S true -T 0.7 -s "$validate" | tee ~cmd.fifo &
chk="$(cat < ~cmd.fifo)"

rm -f ~cmd.fifo

if [[ $chk == "[OK]" ]]
then
	cmd="$gen"
elif [[ $chk == "[EDIT] "* ]]
then
	cmd="${chk:7}"
elif [[ $chk == "[ERR] "* ]]
then
	exit 1
else
	echo "error: validation stage did not use the correct prefix." >&2
	exit 1
fi

while true
do
	echo
	read -n 1 -p "Execute? [Y/n]: " ans
	echo

	if [[ $ans =~ ^[Yy]$ ]] || [[ -z $ans ]]
	then
		printf "$cmd" | bash
		exit 0
	elif [[ $ans =~ ^[Nn]$ ]]; then
		exit 1
	else
		echo "Invalid input."
	fi
done
