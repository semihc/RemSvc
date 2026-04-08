from diagrams import Diagram, Cluster, Edge
from diagrams.onprem.workflow import Airflow
from diagrams.onprem.database import PostgreSQL
from diagrams.onprem.queue import Celery
from diagrams.onprem.client import User
from diagrams.programming.language import Python
from diagrams.generic.storage import Storage
from diagrams.onprem.compute import Server

graph_attr = {
    "fontsize": "14",
    "bgcolor": "white",
    "pad": "0.5",
    "splines": "ortho",
}

with Diagram(
    "Airflow + RemSvc Architecture",
    show=False,
    filename="/home/claude/airflow_remsvc",
    direction="TB",
    graph_attr=graph_attr,
):
    users = User("Users")
    metastore = PostgreSQL("Metastore")

    with Cluster("Airflow"):
        api_server = Airflow("API Server")

        with Cluster("Planning"):
            dag_processor = Airflow("DAG Processor")
            scheduler = Airflow("Scheduler")

        with Cluster("Execution Zone"):
            triggerer = Airflow("Triggerer")
            queue = Celery("Queue")
            workers = Airflow("Workers")

    with Cluster("DAG Folder"):
        dag_folder = Storage("DAG Folder")
        dag_files = [
            Python("dag_1.py"),
            Python("dag_2.py"),
            Python("dag_3.py"),
        ]

    with Cluster("Remote Node"):
        remsvc = Server("RemSvc\ngRPC :50051")

    # Data flows
    users >> Edge(label="monitor") >> api_server
    users >> Edge(label="write DAGs") >> dag_folder

    api_server >> Edge(label="store/retrieve") >> metastore
    metastore >> api_server

    api_server >> dag_processor
    api_server >> scheduler

    scheduler >> Edge(label="schedule") >> queue
    queue >> workers
    workers >> Edge(label="task state") >> api_server

    dag_folder >> dag_processor
    dag_folder >> dag_files

    triggerer >> Edge(
        label="gRPC trigger",
        style="dashed",
        color="darkorange",
    ) >> remsvc
