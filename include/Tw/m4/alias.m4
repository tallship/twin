
define(`TARGET')

include(m4/Tw_alias.m4h)

divert(-1)

define(`set', `define(`base', `$1')`'define(`this', `$2')')
define(`push', `pushdef(`base', `$1')`'pushdef(`this', `$2')')
define(`pop', `popdef(`base')`'popdef(`this')')

define(`el', `set($1,$1)`'methods$1($1, $1)')

define(`extends', `push($1,$2)`'methods$1($1, $2)`'pop()')

define(`field', `ifelse(this, base, `', `
`#'define Tw_$1`'this			Tw_$1`'base')')

define(`alias', `
`#'define Tw_$1			Tw_$2`'this')

divert

Twlist()
