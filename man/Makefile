all: cscout.pdf cswc.pdf csmake.pdf

%.pdf: %.1
	groff -Tps -man <$< >$$.ps
	ps2pdf $$.ps $@
	rm $$.ps
