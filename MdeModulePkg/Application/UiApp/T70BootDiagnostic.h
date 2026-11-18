// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included only by the T70 FrontPage. Read-only HII text, no question callbacks.
STATIC EFI_STRING_ID mT70DiagnosticTokens[6];

STATIC VOID
ResetT70DiagnosticStrings (VOID)
{
  ZeroMem (mT70DiagnosticTokens, sizeof (mT70DiagnosticTokens));
}

STATIC VOID
AppendT70BootDiagnostic (EFI_HII_HANDLE Hii, VOID *OpCodes)
{
  UINT32 Values[5];
  CHAR16 Lines[6][20] = {
    L"T70 D1 (snapshot)", L"I=0000", L"O=0000", L"F=0000",
    L"P=00000000", L"E=00000000"
  };
  CONST CHAR16 Hex[] = L"0123456789ABCDEF";
  CONST UINTN Offsets[5] = {0x818, 0x81c, 0x82c, 0x840, 0x844};
  UINT32 Sequence;
  if (Hii == NULL || OpCodes == NULL ||
      MmioRead64 (0x43F7F000) != 0x0000000148303754ULL ||
      MmioRead64 (0x43F7F800) != 0x000000074C443754ULL ||
      MmioRead32 (0x43F7F808) != 0x43F7F060U ||
      MmioRead32 (0x43F7F80C) != 0) return;
  Sequence = MmioRead32 (0x43F7F814);
  for (UINTN Index = 0; Index < 5; Index++)
    Values[Index] = MmioRead32 (0x43F7F000 + Offsets[Index]);
  MemoryFence ();
  if (MmioRead64 (0x43F7F800) != 0x000000074C443754ULL ||
      MmioRead32 (0x43F7F814) != Sequence) return;
  for (UINTN Index = 0; Index < 5; Index++) {
    UINTN Digits = Index < 3 ? 4 : 8;
    UINT32 Value = Index < 3 ? MIN (Values[Index], 0xffffU) : Values[Index];
    while (Digits != 0) {
      Lines[Index + 1][2 + --Digits] = Hex[Value & 15U];
      Value >>= 4;
    }
  }
  // Reuse package-owned strings on form refresh. Failed updates omit the
  // diagnostic rather than showing a mix of fresh and stale fields.
  for (UINTN Index = 0; Index < 6; Index++) {
    EFI_STRING_ID Token = HiiSetString (Hii, mT70DiagnosticTokens[Index], Lines[Index], NULL);
    if (Token == 0) return;
    mT70DiagnosticTokens[Index] = Token;
  }
  for (UINTN Index = 0; Index < 6; Index++)
    if (HiiCreateTextOpCode (OpCodes, mT70DiagnosticTokens[Index], 0, 0) == NULL) return;
}
