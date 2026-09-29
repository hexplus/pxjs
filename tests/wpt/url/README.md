# Web Platform Tests: URL data

From [web-platform-tests/wpt](https://github.com/web-platform-tests/wpt) at commit
`43681d686ca43bfb9ae5e0bb9e775dc25246d9c4`, unchanged:

- `urltestdata.json`: parsing (`url/resources/urltestdata.json`);
- `setters_tests.json`: the URL setters (`url/resources/setters_tests.json`).

`tools/url_wpt.py` runs them against PXJS's `URL`. Licensed under the 3-clause BSD license in
[LICENSE.md](LICENSE.md). To update: download both files from a newer commit, change the commit
above, run the tool, and fix or list (in `../url-expectations.txt`) any new failure.
