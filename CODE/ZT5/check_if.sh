#!/bin/bash
awk 'BEGIN{d=0} /^[[:space:]]*if /{d++;print NR": "d" if: "$0} /^[[:space:]]*fi$/{print NR": "d" fi: "$0; d--}' "$1"
