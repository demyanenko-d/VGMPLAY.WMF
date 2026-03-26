pyz80 = /Users/ascii/unix/src/pyz80/sourceforge/pyz80/pyz80.py

samflate.dsk: samflate.z80s samflatelib.z80s
	$(pyz80) -o samflate.dsk samflate.z80s

clean:
	rm samflate.dsk

run: samflate.dsk
	open $<


