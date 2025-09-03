#lang racket

(define cxx (find-executable-path "gcc-14"))
(define output-so-path "out/default-plugin.dylib")

(define plugin-path (string-trim (with-output-to-string (lambda () (system* cxx "-print-file-name=plugin")))))

(define args `(
  "-fPIC"
  "-fno-rtti"
  "-shared"
  "-o"
  ,output-so-path
  "src/vector-analysis-claude4.cc"
  "-I"
  ,plugin-path
  "-I"
  ,(build-path plugin-path "include")
  "-undefined" "dynamic_lookup"
))

(define (gmp)
  (append 
    (string-split (string-trim (with-output-to-string
      (lambda () (system "pkg-config --libs gmp")))))
    (string-split (string-trim (with-output-to-string
      (lambda () (system "pkg-config --cflags gmp")))))
  )
)

(define (mpc)
  (define mpc-directory (string-trim (with-output-to-string (lambda () (system "brew --prefix libmpc")))))
  (list "-I" (build-path mpc-directory "include") "-L" (build-path mpc-directory "lib"))
)

(define (mpfr)
  (define mpc-directory (string-trim (with-output-to-string (lambda () (system "brew --prefix mpfr")))))
  (list "-I" (build-path mpc-directory "include") "-L" (build-path mpc-directory "lib"))
)

(set! args (append args (gmp) (mpc) (mpfr)))

(with-handlers ([exn:fail:filesystem? (lambda (_e) (void))])
  (make-directory "out-sh"))
(with-handlers ([exn:fail:filesystem? (lambda (_e) (void))])
  (make-directory "out"))

(call-with-atomic-output-file "out-sh/build.sh" (lambda (o _tmp-path)
  (fprintf o "~a \\~n" cxx)
  (for ([a args])
    (fprintf o "\t~a \\~n" a))
))
(apply system* (cons cxx args))
