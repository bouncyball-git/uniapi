// A check for uniapi_embed()'s minifying (tests/selftest): this comment, the line breaks and
// the long local name below go; what the function does stays. MINIFY-CHECK-COMMENT
export function answer() {
    const aRatherLongLocalVariableName = 40;
    return aRatherLongLocalVariableName + 2;
}
