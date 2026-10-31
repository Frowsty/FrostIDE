# SPDX-License-Identifier: Apache-2.0
#
# A clean PowerShell script for the frost-lint tests.
<#
  TODO(M4): a valid marker in a block comment.
#>
$text = 'TODO: inside a string'
$here = @"
# TODO: inside a here-string
"@
Write-Host $text $here
