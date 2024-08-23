Goofy-ass TODO for Cruft:
1. - [ ] implement arrays at frontend
1. - [ ] create a working qbe backend
1. - [ ] done tab.c and tab.h for purpose
1. - [ ] what the fuck is unsafety! create a secure export method that allows a file (e.g., a.cruft) to be imported only by a specific file (b.cruft). Any other file (e.g., c.cruft) attempting to import it should result in an error
    1. - [ ] if we get secure packages we need async functions or ready function for playing pin-pon with files. e.g., we have a.cruft that specifically uses b.cruft and b.cruft uses a.cruft, that is impossible because no other cause can cause itself and other one when other cause causes the cause and causing itself. for philosophy unlovers: a can't run b when tries to run b and itself at same time and b can't run a and itself at the same time, so, we need to convert the codes to machine code at same time but codes need to wait to get variables that will came from secure package, and secure packages functions need to wait new variables to get ready
        1. - [ ] if there is need async functions that uses another files' variables and results, than make secure packages functions uncallable. e.g., you can't run a.cruft functions in b.cruft or at a.cruft can't run b.cruft functions but you can use they variables and results
1. - [ ] secure, export and to tokens created in .l but never used in .y, implement them
1. - [ ] if there is no secure package then make export and to tokens unusable
1. - [ ] never used vars need to cause warning