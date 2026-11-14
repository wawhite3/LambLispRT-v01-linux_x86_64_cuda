; Copyright 2026 by Frobenius Norm LLC 2026-04-18 00:00:00
(define sandbox-rate-limit      3)
(define sandbox-rate-window-ms  2000)
(define sandbox-whitelist
  '(robot-move!
    robot-stop
    robot-turn!
    robot-sweep
    robot-pose
    set-leds!
    set-led!
    leds-off
    buzzer-beep
    get-sonar
    get-battery
    get-light
    get-heading))
(define sandbox-rate-table '())
(define (sandbox-rate-ok? client-id now-ms)
  (let ((entry (assoc client-id sandbox-rate-table)))
    (if (not entry)
      (begin
        (set! sandbox-rate-table
              (cons (cons client-id (cons 1 now-ms)) sandbox-rate-table))
        #t)
      (let* ((count (cadr entry))
             (win   (cddr entry))
             (age   (- now-ms win)))
        (cond
          ((>= age sandbox-rate-window-ms)
           (set-cdr! entry (cons 1 now-ms))
           #t)
          ((< count sandbox-rate-limit)
           (set-cdr! entry (cons (+ count 1) win))
           #t)
          (else #f))))))
(define sandbox-forbidden-heads
  '(define set! define-syntax load eval apply
    open-input-file open-output-file
    open-tcp-client-port open-tcp-server-port
    system shell))
(define (sandbox-safe? expr)
  (cond
    ((not (pair? expr)) #t)
    ((not (symbol? (car expr))) #f)
    ((memq (car expr) sandbox-forbidden-heads) #f)
    ((not (memq (car expr) sandbox-whitelist)) #f)
    (else
     (let loop ((args (cdr expr)))
       (cond
         ((null? args) #t)
         ((pair? (car args)) (and (sandbox-safe? (car args)) (loop (cdr args))))
         (else (loop (cdr args))))))))
(define (sandbox-eval expr-string client-id car-conn now-ms)
  (cond
    ((not (sandbox-rate-ok? client-id now-ms))
     (list 'error "rate limit exceeded -- please wait"))
    (else
     (let ((expr (guard (e (#t #f))
                   (read (open-input-string expr-string)))))
       (cond
         ((not expr)
          (list 'error "parse error"))
         ((not (sandbox-safe? expr))
          (list 'error "expression not permitted"))
         (else
          (guard (e (#t (list 'error (condition/report-string e))))
            (let ((result (llip-eval car-conn expr)))
              (list 'ok (with-output-to-string (lambda () (write result))))))))))))
