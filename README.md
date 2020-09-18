# chat-server
Operating Systems Project

Things, that were completed:
  1.	Timing now works as expected. In addition, timeout for SENDQUIZ was added: if the group leader doesn’t send quiz in 60 seconds, he/she will be disconnected, and new group won’t be added;
  2.	BAD response messages;
  3.	Now players and questions are freed correctly, after the quiz’s finished or no clients are left during the quiz.
  
Things, that were added:
  1.	I forgot to add mutex in the previous submission. The mutex is required because there are lots of critical sections in the code;
  2.	An array for storing every client’s group index was added, because it reduces the amount of time needed to find client’s group (for LEAVE function). In previous implementation, in order to find client’s group, the program iterated through all groups and clients.
  
  Initially, the plan was to allow to use “group cancel” function only to the group leader, but since it was easier to test the program, those lines were commented. (they are located in cancelGroup function).
