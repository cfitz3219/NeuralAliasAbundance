# Reversible edits to numeric values in ordinary game preferences.
# Keep all other bytes, including the original line endings and encoding.
function Read-ScaleField($Path, $Field) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $encoding = [Text.Encoding]::GetEncoding(28591) # One-to-one byte view, also preserves UTF-8.
    if ($bytes.Length -ge 2) {
        if ($bytes[0] -eq 255 -and $bytes[1] -eq 254) { $encoding = [Text.Encoding]::Unicode }
        if ($bytes[0] -eq 254 -and $bytes[1] -eq 255) { $encoding = [Text.Encoding]::BigEndianUnicode }
    }
    $body = $encoding.GetString($bytes)
    $number = '(?<number>[+-]?\d+(?:\.\d+)?)'
    $key = [regex]::Escape($Field.Key)
    if ($Field.Format -eq 'block') {
        # Only an unambiguous version/value scalar record. Other record shapes are unsupported.
        $pattern = '(?m)^[\t ]*"' + $key + '"\s*=\s*\{\s*(?:version\s*=\s*\d+\s+)?value\s*=\s*' + $number + '\s*\}'
        $matchesFound = [regex]::Matches($body, $pattern)
        $offset = 0
    } elseif ($Field.Format -eq 'ini') {
        $header = [regex]::Escape($Field.Section)
        $sections = [regex]::Matches($body,'(?im)^(?:\uFEFF|ï»¿)?[\t ]*\[' + $header + '\][^\r\n]*\r?\n(?<body>(?:(?!^[\t ]*\[)[\s\S])*)')
        if ($sections.Count -ne 1) { throw "Missing or ambiguous INI section [$($Field.Section)] in $Path" }
        $section = $sections[0].Groups['body']
        $offset = $section.Index
        $matchesFound = [regex]::Matches($section.Value,'(?im)^[\t ]*' + $key + '[\t ]*=[\t ]*' + $number + '[\t ]*(?:[;#][^\r\n]*)?\r?$')
    } else { throw "Unknown preference format: $($Field.Format)" }
    if ($matchesFound.Count -ne 1) { throw "Expected one numeric $($Field.Key) setting in $Path, found $($matchesFound.Count)." }
    $token = $matchesFound[0].Groups['number']
    return @{ Text = $body; Encoding = $encoding; Index = $offset + $token.Index; Original = $token.Value; Bytes = $bytes }
}

function Write-ScaleField($Entry, [bool]$Restore) {
    $expect = $Entry.Original; $replacement = $Entry.Request
    if ($Restore) { $expect = $Entry.Request; $replacement = $Entry.Original }
    $field = Read-ScaleField $Entry.Path $Entry.Field
    if ($field.Original -ceq $replacement) { return }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    if ([decimal]::Parse($field.Original,$culture) -ne [decimal]::Parse($expect,$culture)) {
        if ($Restore) { Write-Host "Kept a newer value for $($Entry.Field.Key)."; return }
        throw "The setting $($Entry.Field.Key) changed before launch."
    }
    $newText = $field.Text.Remove($field.Index,$field.Original.Length).Insert($field.Index,$replacement)
    $temp = $Entry.Path + '.na-' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllBytes($temp,$field.Encoding.GetBytes($newText))
        # Recheck the file immediately before the same-volume atomic swap.
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Entry.Path)) -cne [Convert]::ToBase64String($field.Bytes)) {
            throw "The file changed during the edit. Retry to preserve the latest settings."
        }
        $backup = $temp + '.previous'
        [IO.File]::Replace($temp,$Entry.Path,$backup)
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backup)) -cne [Convert]::ToBase64String($field.Bytes)) {
            throw "Concurrent file replacement. The other writer's version is preserved at $backup."
        }
        [IO.File]::Delete($backup)
    } finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
}

function New-ScaleEdit($Path, $Field, [string]$Request) {
    $full = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $entry = Read-ScaleField $full $Field
    return [pscustomobject]@{ Path = $full; Field = $Field; Original = $entry.Original; Request = $Request }
}
