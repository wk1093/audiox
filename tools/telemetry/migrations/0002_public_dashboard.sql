CREATE TABLE public_dashboard (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    payload TEXT NOT NULL
);
CREATE TABLE public_daily (
    day TEXT PRIMARY KEY,
    payload TEXT NOT NULL
);
