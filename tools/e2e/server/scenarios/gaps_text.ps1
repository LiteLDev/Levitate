# U37 native text fields, server side: the actions the client mod's text fields send (package
# filter address screen, stock keeper search / address, category names) with the forms' limits.
$r = Invoke-SelfTest 'gaps.textfield_*'
if ($r.Pass -lt 2) { throw "expected the 2 gaps.textfield selftests to pass, got $($r.Pass)" }
