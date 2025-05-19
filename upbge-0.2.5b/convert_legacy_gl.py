import os
import re

# Caminho raiz do seu código-fonte
ROOT_DIR = "source"

# Funções legadas e seus substitutos (None = precisa de substituição manual)
LEGACY_FUNCTIONS = {
    "glAccum": None,
    "glAlphaFunc": None,
    "glBegin": None,
    "glBitmap": None,
    "glCallList": None,
    "glClearAccum": None,
    "glClearIndex": None,
    "glClientActiveTexture": None,
    "glClipPlane": None,
    "glColor3b": "glVertexAttrib3f",
    "glColor3bv": "glVertexAttrib3fv",
    "glColor3d": "glVertexAttrib3f",
    "glColor3dv": "glVertexAttrib3fv",
    "glColor3f": "glVertexAttrib3f",
    "glColor3fv": "glVertexAttrib3fv",
    "glColor3i": "glVertexAttrib3f",
    "glColor3iv": "glVertexAttrib3fv",
    "glColor3s": "glVertexAttrib3f",
    "glColor3sv": "glVertexAttrib3fv",
    "glColor3ub": "glVertexAttrib3f",
    "glColor3ubv": "glVertexAttrib3fv",
    "glColor4b": "glVertexAttrib4f",
    "glColor4bv": "glVertexAttrib4fv",
    "glColor4d": "glVertexAttrib4f",
    "glColor4dv": "glVertexAttrib4fv",
    "glColor4f": "glVertexAttrib4f",
    "glColor4fv": "glVertexAttrib4fv",
    "glColor4i": "glVertexAttrib4f",
    "glColor4iv": "glVertexAttrib4fv",
    "glColor4s": "glVertexAttrib4f",
    "glColor4sv": "glVertexAttrib4fv",
    "glColor4ub": "glVertexAttrib4f",
    "glColor4ubv": "glVertexAttrib4fv",
    "glColorMaterial": None,
    "glColorPointer": None,
    "glDeleteLists": None,
    "glDisableLists": None,
    "glDisableClientState": None,
    "glDrawPixels": None,
    "glEdgeFlag": None,
    "glEdgeFlagPointer": None,
    "glEdgeFlagv": None,
    "glEnableClientState": None,
    "glEndList": None,
}

manual_needed = []

def process_files():
    for root, _, files in os.walk(ROOT_DIR):
        for file in files:
            if file.endswith((".c", ".cpp", ".h")):
                filepath = os.path.join(root, file)
                with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()

                new_content = content
                for func, replacement in LEGACY_FUNCTIONS.items():
                    pattern = re.compile(rf'\b(DO_NOT_USE_)?{func}\b')
                    if replacement:
                        new_content = pattern.sub(replacement, new_content)
                    elif pattern.search(new_content):
                        manual_needed.append((func, filepath))

                if new_content != content:
                    with open(filepath, "w", encoding="utf-8") as f:
                        f.write(new_content)

    print("\nFunções que precisam de conversão manual:")
    for func, path in sorted(set(manual_needed)):
        print(f" - {func} em {path}")

process_files()
