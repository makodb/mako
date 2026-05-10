.PHONY: pdf check clean open

pdf:
	latexmk -pdf main.tex

check: pdf
	pdfinfo main.pdf
	pdffonts main.pdf
	@pdffonts main.pdf | awk 'NR > 2 && $$5 != "yes" { bad=1 } END { exit bad }'
	@if grep -q "LaTeX Warning: There were undefined references" main.log; then exit 1; fi
	@if grep -q "Citation .* undefined" main.log; then exit 1; fi
	@if grep -q "There were undefined citations" main.log; then exit 1; fi

clean:
	latexmk -C main.tex
	rm -f *.bbl *.bcf *.run.xml *.xmpi *.fls *.fdb_latexmk

open:
	xdg-open main.pdf >/dev/null 2>&1 &
