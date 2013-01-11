============
vmod_rewrite
============

----------------------
Varnish Rewrite Module
----------------------

:Author: Aivars Kalvans <aivars.kalvans@gmail.com>
:Date: 2013-01-18
:Version: 0.1
:Manual section: 3

SYNOPSIS
========

import rewrite;

DESCRIPTION
===========

Varnish vmod hack demonstrating how to rewrite HTML content. It's not
production-ready - I'm still learning and looking for the best way how
to do it.

`Buy me a drink!`__

__ https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=FUPUJSJ9KCPAL&lc=LV&item_name=libvmod%2drewrite&currency_code=USD&bn=PP%2dDonationsBF%3abtn_donate_SM%2egif%3aNonHosted

FUNCTIONS
=========

rewrite_r
---------

Prototype
        ::

                rewrite_re(STRING SEARCH_REGEX, STRING REPLACEMENT)
Return value
	VOID
Description
	Rewrites all parts of document matching SEARCH_REGEX with REPLACEMENT


INSTALLATION
============

Usage::

 ./configure VARNISHSRC=DIR [VMODDIR=DIR]

`VARNISHSRC` is the directory of the Varnish source tree for which to
compile your vmod. Both the `VARNISHSRC` and `VARNISHSRC/include`
will be added to the include search paths for your module.

Optionally you can also set the vmod install directory by adding
`VMODDIR=DIR` (defaults to the pkg-config discovered directory from your
Varnish installation).

Make targets:

* make - builds the vmod
* make install - installs your vmod in `VMODDIR`

In your VCL you could then use this vmod along the following lines::
        
        import rewrite;

        sub vcl_deliver {
                rewrite.rewrite_re("https://www.paypal.com/cgi-bin/webscr\?cmd=_donations&business=[^&]+&", "https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=FUPUJSJ9KCPAL&");
        }

(No I'm not that evil)

COPYRIGHT
=========

See COPYING for details.

* Copyright (c) 2013 Aivars Kalvans <aivars.kalvans@gmail.com>
