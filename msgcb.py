import re
RULES = [
    (r"mb252_unit", "corpus_alpha"),
    (r"read_hdl_mb252", "read_hdl_corpus_alpha"),
    (r"mb210_update_probe", "corpus_beta_update_probe"),
    (r"\bmb252\b", "corpus_alpha"),
    (r"\bmb210\b", "corpus_beta"),
]
def message_callback(message):
    text = message.decode("utf-8", "replace")
    for pattern, repl in RULES:
        text = re.sub(pattern, repl, text, flags=re.I)
    return text.encode("utf-8")
