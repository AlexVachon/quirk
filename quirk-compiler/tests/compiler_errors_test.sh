#!/bin/bash
# Shows what the improved compiler error messages look like.
QUIRK=./bin/quirk

echo "============================================"
echo " 1. Undefined variable"
echo "============================================"
$QUIRK <(cat <<'EOF'
define main() {
    y := z + 1
}
EOF
) 2>&1; echo ""

echo "============================================"
echo " 2. 'while' condition must be Bool"
echo "============================================"
$QUIRK <(cat <<'EOF'
define main() {
    x := 5
    while x {
        print("looping")
        x := x - 1
    }
}
EOF
) 2>&1; echo ""

echo "============================================"
echo " 3. Member not found"
echo "============================================"
$QUIRK <(cat <<'EOF'
define main() {
    x := 42
    print(x.nonexistent())
}
EOF
) 2>&1; echo ""

echo "============================================"
echo " 4. --check on a valid file"
echo "============================================"
$QUIRK --check tests/lists.qk 2>&1; echo ""

echo "============================================"
echo " 5. --check on an invalid file"
echo "============================================"
$QUIRK --check <(cat <<'EOF'
define main() {
    result := missing_func()
}
EOF
) 2>&1; echo ""

echo "============================================"
echo " 6. 'if' condition must be Bool"
echo "============================================"
$QUIRK <(cat <<'EOF'
define main() {
    x := 42
    if x {
        print("yes")
    }
}
EOF
) 2>&1; echo ""
