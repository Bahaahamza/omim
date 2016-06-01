#|
exec /usr/bin/env sbcl --noinform --quit --eval "(defparameter *script-name* \"$0\")" --load "$0" --end-toplevel-options "$@"
|#

;;; This script clusterizes values from the taginfo database and
;;; prints information about clusters.

;;; Silently loads sqlite.
(with-open-file (*standard-output* "/dev/null"
                                   :direction :output
                                   :if-exists :supersede)
  (ql:quickload "sqlite"))

(defun latin-char-p (char)
  (or (and (char>= char #\a) (char<= char #\z))
      (and (char>= char #\A) (char<= char #\Z))))

(defun starts-with (text prefix)
  "Returns non-nil if text starts with prefix."
  (let ((pos (search prefix text)))
    (and pos (= 0 pos))))

(defun get-postcode-pattern (postcode fn)
  "Simplifies postcode in the following way:
   * all latin letters are replaced by 'A'
   * all digits are replaced by 'N'
   * hyphens and dots are replaced by a space
   * other characters are capitalized

   This format follows https://en.wikipedia.org/wiki/List_of_postal_codes.
  "
  (funcall fn postcode (map 'string #'(lambda (c) (cond ((latin-char-p c) #\A)
                                                        ((digit-char-p c) #\N)
                                                        ((or (char= #\- c) (char= #\. c)) #\Space)
                                                        (T c)))
                            (string-upcase postcode))))


(defun get-phone-or-flat-pattern (phone fn)
  "Simplifies phone or flat numbers in the following way:
   * all letters are replaced by 'A'
   * all digits are replaced by 'N'
   * other characters are capitalized
  "
  (funcall fn phone (map 'string #'(lambda (c) (cond ((alpha-char-p c) #\A)
                                                     ((digit-char-p c) #\N)
                                                     (T c)))
                         (string-upcase phone))))

(defun group-by (fn list)
  "fn -> [a] -> [[a]]

   Groups equal adjacent elements of the list. Equality is checked with fn.
  "
  (let ((buckets
           (reduce #'(lambda (buckets cur)
                       (cond ((null buckets) (cons (list cur) buckets))
                             ((funcall fn (caar buckets) cur)
                              (cons (cons cur (car buckets)) (cdr buckets)))
                             (T (cons (list cur) buckets))))
                   list :initial-value nil)))
    (reverse (mapcar #'reverse buckets))))

(defun split-by (fn list)
  "fn -> [a] -> [[a]]

   Splits list by separators, where separators are defined by fn
   predicate.
  "
  (loop for e in list
     with buckets = nil
     for prev-sep = T then cur-sep
     for cur-sep = (funcall fn e)
     do (cond (cur-sep T)
              (prev-sep (push (list e) buckets))
              (T (push e (car buckets))))
     finally (return (reverse (mapcar #'reverse buckets)))))

(defun split-string-by (fn string)
  "fn -> string -> [string]

   Splits string by separators, where separators are defined by fn
   predicate.
  "
  (mapcar #'(lambda (list) (concatenate 'string list))
          (split-by fn (concatenate 'list string))))

(defun drop-while (fn list)
  (cond ((null list) nil)
        ((funcall fn (car list)) (drop-while fn (cdr list)))
        (T list)))

(defparameter *building-synonyms*
  '("building" "bldg" "bld" "bl" "unit" "block" "blk"
    "корпус" "корп" "литер" "лит" "строение" "блок" "бл"))

(defparameter *house-number-seps* '(#\Space #\. #\( #\) #\# #\~))
(defparameter *house-number-groups-seps* '(#\, #\| #\; #\+))

(defun building-synonym-p (s)
  (find s *building-synonyms* :test #'string=))

(defun short-building-synonym-p (s)
  (or (string= "к" s) (string= "с" s)))

(defstruct token value type)

(defun get-char-type (c)
  (cond ((digit-char-p c) :number)
        ((find c *house-number-seps* :test #'char=) :separator)
        ((find c *house-number-groups-seps* :test #'char=) :group-separator)
        ((char= c #\-) :hyphen)
        ((char= c #\/) :slash)
        (T :string)))

(defun tokenize-house-number (house-number)
  "house-number => [token]"
  (let ((parts (group-by #'(lambda (lhs rhs)
                             (eq (get-char-type lhs) (get-char-type rhs)))
                         (string-downcase house-number))))
    (remove-if #'(lambda (token) (eq :separator (token-type token)))
               (mapcar #'(lambda (part) (make-token :value (concatenate 'string part)
                                                    :type (get-char-type (car part))))
                       parts))))

(defun house-number-with-optional-suffix-p (tokens)
  (case (length tokens)
    (1 (eq (token-type (first tokens)) :number))
    (2 (and (eq (token-type (first tokens)) :number)
            (eq (token-type (second tokens)) :string)))
    (otherwise nil)))

(defun get-house-number-sub-numbers (house-number)
  "house-number => [[token]]

   As house-number can be actually a collection of separated house
   numbers, this function returns a list of possible house numbers.
   Current implementation splits house number if and only if
   house-number matches the following rule:

   NUMBERS ::= (NUMBER STRING-SUFFIX?) | (NUMBER STRING-SUFFIX?) SEP NUMBERS
  "
  (let* ((tokens (tokenize-house-number house-number))
         (groups (split-by #'(lambda (token) (eq :group-separator (token-type token))) tokens)))
    (if (every #'house-number-with-optional-suffix-p groups)
        groups
        (list tokens))))

(defun parse-house-number (tokens)
  "[token] => [token]

   Parses house number, but as the grammar is undefined and ambiguous,
   the parsing is just a split of some tokens, i.e. 'литА' will be split
   to building synonym (литер) and to letter (A).
  "
  (loop with result = (list)
     for token in tokens
     for token-value = (token-value token)
     for token-type = (token-type token)
     do (case token-type
          (:string (cond ((building-synonym-p token-value)
                          (push (make-token :value token-value
                                            :type :building-part)
                                result))
                         ((and (= 4 (length token-value))
                               (starts-with token-value "лит"))
                          (push (make-token :value (subseq token-value 0 3)
                                            :type :building-part)
                                result)
                          (push (make-token :value (subseq token-value 3)
                                            :type :letter)
                                result))
                        ((and (= 2 (length token-value))
                              (short-building-synonym-p (subseq token-value 0 1)))
                         (push (make-token :value (subseq token-value 0 1)
                                           :type :building-part)
                               result)
                         (push (make-token :value (subseq token-value 1)
                                           :type :letter)
                               result))
                        ((= 1 (length token-value))
                         (push (make-token :value token-value
                                           :type (if (short-building-synonym-p token-value)
                                                     :letter-or-building-part
                                                     :letter))
                               result))
                        (T (push token result))))
                   (otherwise (push token result)))
     finally (return (reverse result))))

(defun join-house-number-tokens (tokens)
  "Joins token values with spaces."
  (format nil "~{~a~^ ~}" (mapcar #'token-value tokens)))

(defun join-house-number-parse (tokens)
  "Joins parsed house number tokens with spaces."
  (format nil "~{~a~^ ~}"
          (mapcar #'(lambda (token)
                      (let ((token-type (token-type token))
                            (token-value (token-value token)))
                        (case token-type
                          (:number "N")
                          (:building-part "B")
                          (:letter "L")
                          ((:string :letter-or-building-part :hyphen :slash :group-separator)
                           token-value)
                          (otherwise (assert NIL NIL (format nil "Unknown token type: ~a"
                                                             token-type))))))
                  tokens)))

(defun get-house-number-pattern (house-number fn)
  (dolist (number (get-house-number-sub-numbers house-number))
    (funcall fn (join-house-number-tokens number)
             (join-house-number-parse (drop-while #'(lambda (token)
                                                      (not (eq :number (token-type token))))
                                                  (parse-house-number number))))))

(defun get-house-number-strings (house-number fn)
  (dolist (number (get-house-number-sub-numbers house-number))
    (dolist (string (mapcar #'token-value
                            (remove-if-not #'(lambda (token)
                                               (let ((token-type (token-type token)))
                                                 (or (eq :string token-type)
                                                     (eq :letter token-type)
                                                     (eq :letter-or-building-part token-type))))
                                           (parse-house-number number))))
      (funcall fn string string))))

(defstruct type-settings
  pattern-simplifier
  field-name)

(defparameter *value-type-settings*
  `(:postcode ,(make-type-settings :pattern-simplifier #'get-postcode-pattern
                                   :field-name "addr:postcode")
              :phone ,(make-type-settings :pattern-simplifier #'get-phone-or-flat-pattern
                                          :field-name "contact:phone")
              :flat ,(make-type-settings :pattern-simplifier #'get-phone-or-flat-pattern
                                         :field-name "addr:flats")
              :house-number ,(make-type-settings :pattern-simplifier #'get-house-number-pattern
                                                 :field-name "addr:housenumber")
              :house-number-strings ,(make-type-settings
                                      :pattern-simplifier #'get-house-number-strings
                                      :field-name "addr:housenumber")))

(defstruct cluster
  "A cluster of values with the same pattern, i.e.  all six-digits
   series or all four-digits-two-letters series."
  (key "") (num-samples 0) (samples nil))

(defun add-sample (cluster sample &optional (count 1))
  "Adds a value sample to a cluster of samples."
  (push sample (cluster-samples cluster))
  (incf (cluster-num-samples cluster) count))

(defparameter *seps* '(#\Space #\Tab #\Newline #\Backspace #\Return #\Rubout #\Linefeed #\"))

(defun trim (string)
  "Removes leading and trailing garbage from a string."
  (string-trim *seps* string))

(defun get-pattern-clusters (values simplifier)
  "Constructs a list of clusters by a list of values."
  (let ((table (make-hash-table :test #'equal))
        (clusters nil))
    (loop for (value count) in values
       do (funcall simplifier (trim value)
                   #'(lambda (value pattern)
                       (let ((cluster (gethash pattern table (make-cluster :key pattern))))
                         (add-sample cluster value count)
                         (setf (gethash pattern table) cluster)))))
    (maphash #'(lambda (pattern cluster)
                 (declare (ignore pattern))
                 (push cluster clusters))
             table)
    clusters))

(defun make-keyword (name) (values (intern (string-upcase name) "KEYWORD")))

(when (/= 3 (length *posix-argv*))
  (format t "Usage: ~a ~{~a~^|~} path-to-taginfo-db.db~%"
          *script-name*
          (loop for field in *value-type-settings* by #'cddr collecting field))
  (exit :code -1))

(defparameter *value-type* (second *posix-argv*))
(defparameter *db-path* (third *posix-argv*))

(defparameter *type-settings* (getf *value-type-settings* (make-keyword *value-type*)))

(defparameter *values*
  (sqlite:with-open-database (db *db-path*)
    (let ((query (format nil "select value, count_all from tags where key=\"~a\";"
                         (type-settings-field-name *type-settings*))))
      (sqlite:execute-to-list db query))))

(defparameter *clusters*
  (sort (get-pattern-clusters *values* (type-settings-pattern-simplifier *type-settings*))
        #'(lambda (lhs rhs) (> (cluster-num-samples lhs)
                               (cluster-num-samples rhs)))))

(defparameter *total*
  (loop for cluster in *clusters*
     summing (cluster-num-samples cluster)))

(format t "Total: ~a~%" *total*)
(loop for cluster in *clusters*
   for prev-prefix-sum = 0 then curr-prefix-sum
   for curr-prefix-sum = (+ prev-prefix-sum (cluster-num-samples cluster))
   do (let ((key (cluster-key cluster))
            (num-samples (cluster-num-samples cluster))
            (samples (cluster-samples cluster)))
        ; Prints number of values in a cluster, accumulated
        ; percent of values clustered so far, simplified version
        ; of a value and examples of values.
        (format t "~a (~2$%) ~a [~{~a~^, ~}~:[~;, ...~]]~%"
                num-samples
                (* 100 (/ curr-prefix-sum *total*))
                key
                (subseq samples 0 (min (length samples) 5))
                (> num-samples 5))))
