import psycopg2
import psycopg2.extras

from . import config


def get_connection():
    return psycopg2.connect(
        host=config.POSTGRES_HOST,
        port=config.POSTGRES_PORT,
        dbname=config.POSTGRES_DB,
        user=config.POSTGRES_USER,
        password=config.POSTGRES_PASSWORD,
        cursor_factory=psycopg2.extras.RealDictCursor,
    )
